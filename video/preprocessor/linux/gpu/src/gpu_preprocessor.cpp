// preprocessor/gpu/src/gpu_preprocessor.cpp
// GPU-accelerated BGRA→NV12 format conversion using VA-API VPP.
// Falls back to CPU conversion (same as CpuPreprocessor) when VA-API VPP
// is not available on the current hardware/driver combination.

#include "gpu_preprocessor.h"

#include <cstring>
#include <iostream>

#include <va/va.h>
#include <va/va_drm.h>
#include <va/va_vpp.h>
#include <fcntl.h>
#include <unistd.h>
#include <cstdlib>

namespace pulsar::preprocessor::gpu {

namespace {

bool open_vpp_display(VADisplay& out, int& out_fd) {
    out_fd = ::open("/dev/dri/renderD128", O_RDWR | O_CLOEXEC);
    if (out_fd < 0) return false;
    out = vaGetDisplayDRM(out_fd);
    if (!out) { ::close(out_fd); out_fd=-1; return false; }
    static const char* kDrivers[] = {"nvidia","iHD","i965","radeonsi","nouveau",nullptr};
    const char* prev = std::getenv("LIBVA_DRIVER_NAME");
    const std::string prev_str = prev ? prev : "";
    int maj=0, min_=0; bool ok=false;
    for (const char** d=kDrivers; *d; ++d) {
        ::setenv("LIBVA_DRIVER_NAME",*d,1);
        if (vaInitialize(out,&maj,&min_)==VA_STATUS_SUCCESS){ok=true;break;}
    }
    if (prev) ::setenv("LIBVA_DRIVER_NAME",prev_str.c_str(),1);
    else      ::unsetenv("LIBVA_DRIVER_NAME");
    if (!ok){ ::close(out_fd); out_fd=-1; }
    return ok;
}

// CPU fallback: de-interleave BGRA→NV12
void cpu_bgra_to_nv12(const uint8_t* bgra, uint8_t* nv12, int w, int h) {
    // Y = 0.299R + 0.587G + 0.114B  (BT.601 approximate)
    // U = -0.169R - 0.331G + 0.5B + 128
    // V =  0.5R  - 0.419G - 0.081B + 128
    uint8_t* Y  = nv12;
    uint8_t* UV = nv12 + w * h;
    for (int row=0; row<h; ++row) {
        for (int col=0; col<w; ++col) {
            const uint8_t* p = bgra + (row*w+col)*4;
            uint8_t B=p[0], G=p[1], R=p[2];
            Y[row*w+col] = static_cast<uint8_t>(
                (66*R + 129*G + 25*B + 128) >> 8 + 16);
            if ((row&1)==0 && (col&1)==0) {
                int idx = (row/2)*w + col;
                UV[idx]   = static_cast<uint8_t>(
                    ((-38*R - 74*G + 112*B + 128) >> 8) + 128);
                UV[idx+1] = static_cast<uint8_t>(
                    ((112*R - 94*G -  18*B + 128) >> 8) + 128);
            }
        }
    }
}

} // namespace

// ─── GpuPreprocessor ──────────────────────────────────────────────────────────
struct GpuPreprocessor::Impl {
    VADisplay   display    = nullptr;
    int         drm_fd     = -1;
    VAConfigID  vpp_config = VA_INVALID_ID;
    VAContextID vpp_ctx    = VA_INVALID_ID;
    VASurfaceID src_surf   = VA_INVALID_SURFACE;
    VASurfaceID dst_surf   = VA_INVALID_SURFACE;
    int         width      = 0;
    int         height     = 0;
    bool        vpp_ready  = false;
};

GpuPreprocessor::GpuPreprocessor() : impl_(std::make_unique<Impl>()) {
    if (open_vpp_display(impl_->display, impl_->drm_fd)) {
        std::cerr << "[gpu_preprocessor] VA-API VPP available\n";
    } else {
        std::cerr << "[gpu_preprocessor] VA-API VPP not available — CPU fallback\n";
    }
}

GpuPreprocessor::~GpuPreprocessor() {
    close_vpp();
    if (impl_->display) { vaTerminate(impl_->display); impl_->display=nullptr; }
    if (impl_->drm_fd>=0) { ::close(impl_->drm_fd); impl_->drm_fd=-1; }
}

void GpuPreprocessor::close_vpp() {
    if (!impl_||!impl_->display) return;
    if (impl_->dst_surf  !=VA_INVALID_SURFACE){ vaDestroySurfaces(impl_->display,&impl_->dst_surf,1);  impl_->dst_surf=VA_INVALID_SURFACE; }
    if (impl_->src_surf  !=VA_INVALID_SURFACE){ vaDestroySurfaces(impl_->display,&impl_->src_surf,1);  impl_->src_surf=VA_INVALID_SURFACE; }
    if (impl_->vpp_ctx   !=VA_INVALID_ID)     { vaDestroyContext(impl_->display,impl_->vpp_ctx);       impl_->vpp_ctx=VA_INVALID_ID; }
    if (impl_->vpp_config!=VA_INVALID_ID)     { vaDestroyConfig(impl_->display,impl_->vpp_config);     impl_->vpp_config=VA_INVALID_ID; }
    impl_->vpp_ready=false;
}

bool GpuPreprocessor::open_vpp(int w, int h) {
    close_vpp();
    Impl& m = *impl_;
    if (!m.display) return false;

    if (vaCreateConfig(m.display, VAProfileNone, VAEntrypointVideoProc,
                       nullptr, 0, &m.vpp_config) != VA_STATUS_SUCCESS)
        return false;

    // BGRA source surface
    VASurfaceAttrib sa{};
    sa.type=VASurfaceAttribPixelFormat; sa.flags=VA_SURFACE_ATTRIB_SETTABLE;
    sa.value.type=VAGenericValueTypeInteger; sa.value.value.i=VA_FOURCC_BGRA;
    if (vaCreateSurfaces(m.display, VA_RT_FORMAT_RGB32,
                         static_cast<unsigned>(w), static_cast<unsigned>(h),
                         &m.src_surf, 1, &sa, 1) != VA_STATUS_SUCCESS) {
        vaDestroyConfig(m.display, m.vpp_config); m.vpp_config=VA_INVALID_ID;
        return false;
    }

    // NV12 destination surface
    sa.value.value.i = VA_FOURCC_NV12;
    if (vaCreateSurfaces(m.display, VA_RT_FORMAT_YUV420,
                         static_cast<unsigned>(w), static_cast<unsigned>(h),
                         &m.dst_surf, 1, &sa, 1) != VA_STATUS_SUCCESS) {
        close_vpp(); return false;
    }

    if (vaCreateContext(m.display, m.vpp_config, w, h,
                        VA_PROGRESSIVE, &m.dst_surf, 1,
                        &m.vpp_ctx) != VA_STATUS_SUCCESS) {
        close_vpp(); return false;
    }

    m.width=w; m.height=h; m.vpp_ready=true;
    return true;
}

std::optional<pulsar::core::RawFrame>
GpuPreprocessor::process(pulsar::core::RawFrame frame)
{
    if (!frame.buffer || frame.width<=0 || frame.height<=0) return std::nullopt;

    // Already NV12 — pass through
    if (frame.format == pulsar::core::PixelFormat::NV12) return frame;

    const int w = frame.width, h = frame.height;

    // ── Try VA-API VPP ────────────────────────────────────────────────────────
    if (impl_->display) {
        if (!impl_->vpp_ready || impl_->width!=w || impl_->height!=h)
            open_vpp(w, h);

        if (impl_->vpp_ready) {
            Impl& m = *impl_;
            // Upload BGRA to src_surf via VAImage
            VAImage img{}; void* ptr=nullptr;
            VAImageFormat bgra_fmt{};
            bgra_fmt.fourcc = VA_FOURCC_BGRA;
            bgra_fmt.byte_order = VA_LSB_FIRST;
            bgra_fmt.bits_per_pixel = 32;
            if (vaCreateImage(m.display, &bgra_fmt, w, h, &img) == VA_STATUS_SUCCESS) {
                vaMapBuffer(m.display,img.buf,&ptr);
                auto* dst=static_cast<uint8_t*>(ptr);
                for (int row=0;row<h;++row)
                    std::memcpy(dst+img.offsets[0]+row*static_cast<int>(img.pitches[0]),
                                frame.buffer->data()+row*w*4, static_cast<size_t>(w*4));
                vaUnmapBuffer(m.display,img.buf);
                vaPutImage(m.display,m.src_surf,img.image_id,0,0,
                           static_cast<unsigned>(w),static_cast<unsigned>(h),
                           0,0,static_cast<unsigned>(w),static_cast<unsigned>(h));
                vaDestroyImage(m.display,img.image_id);

                // VPP: colour-space conversion BGRA→NV12
                VAProcPipelineParameterBuffer pp{};
                pp.surface                    = m.src_surf;
                pp.surface_color_standard     = VAProcColorStandardBT601;
                pp.output_color_standard      = VAProcColorStandardBT601;
                pp.output_background_color    = 0xFF000000;
                VARectangle region{0,0,static_cast<uint16_t>(w),static_cast<uint16_t>(h)};
                pp.surface_region = &region;
                pp.output_region  = &region;
                VABufferID pp_buf=VA_INVALID_ID;
                vaCreateBuffer(m.display,m.vpp_ctx,VAProcPipelineParameterBufferType,sizeof(pp),1,&pp,&pp_buf);
                vaBeginPicture(m.display,m.vpp_ctx,m.dst_surf);
                vaRenderPicture(m.display,m.vpp_ctx,&pp_buf,1);
                vaEndPicture(m.display,m.vpp_ctx);
                vaSyncSurface(m.display,m.dst_surf);
                vaDestroyBuffer(m.display,pp_buf);

                // Download NV12 from dst_surf
                VAImage out_img{}; void* out_ptr=nullptr;
                if (vaDeriveImage(m.display,m.dst_surf,&out_img)==VA_STATUS_SUCCESS) {
                    vaMapBuffer(m.display,out_img.buf,&out_ptr);
                    size_t nv12_size=static_cast<size_t>(w*h*3/2);
                    struct Buf final: public pulsar::core::FrameBuffer {
                        std::vector<uint8_t> d_;
                        Buf(size_t n):d_(n){}
                        uint8_t* data() const override{return const_cast<uint8_t*>(d_.data());}
                        size_t size() const override{return d_.size();}
                    };
                    auto out_buf=std::make_shared<Buf>(nv12_size);
                    auto* s=static_cast<uint8_t*>(out_ptr);
                    for (int row=0;row<h;++row)
                        std::memcpy(out_buf->d_.data()+row*w, s+out_img.offsets[0]+row*static_cast<int>(out_img.pitches[0]), static_cast<size_t>(w));
                    for (int row=0;row<h/2;++row)
                        std::memcpy(out_buf->d_.data()+w*h+row*w, s+out_img.offsets[1]+row*static_cast<int>(out_img.pitches[1]), static_cast<size_t>(w));
                    vaUnmapBuffer(m.display,out_img.buf);
                    vaDestroyImage(m.display,out_img.image_id);

                    frame.buffer = std::move(out_buf);
                    frame.format = pulsar::core::PixelFormat::NV12;
                    frame.color_info.color_space = pulsar::core::ColorSpace::BT709;
                    return frame;
                } // end vaDeriveImage
            } // end vaCreateImage
        } // end vpp_ready
    } // end display ──────────────────────────────────────────────────────────
    struct CpuBuf final: public pulsar::core::FrameBuffer {
        std::vector<uint8_t> d_;
        CpuBuf(size_t n):d_(n,0){}
        uint8_t* data() const override{return const_cast<uint8_t*>(d_.data());}
        size_t size() const override{return d_.size();}
    };
    size_t sz=static_cast<size_t>(w*h*3/2);
    auto cpu_buf=std::make_shared<CpuBuf>(sz);
    if (frame.format==pulsar::core::PixelFormat::BGRA)
        cpu_bgra_to_nv12(frame.buffer->data(), cpu_buf->d_.data(), w, h);
    else {
        // RGBA: swap B and R then convert
        std::vector<uint8_t> tmp(static_cast<size_t>(w*h*4));
        const uint8_t* src=frame.buffer->data();
        for (int i=0;i<w*h;++i){
            tmp[i*4+0]=src[i*4+2]; tmp[i*4+1]=src[i*4+1];
            tmp[i*4+2]=src[i*4+0]; tmp[i*4+3]=src[i*4+3];
        }
        cpu_bgra_to_nv12(tmp.data(), cpu_buf->d_.data(), w, h);
    }
    frame.buffer = std::move(cpu_buf);
    frame.format = pulsar::core::PixelFormat::NV12;
    frame.color_info.color_space = pulsar::core::ColorSpace::BT709;
    return frame;
}

pulsar::core::AdapterCapabilities GpuPreprocessor::capabilities() const {
    pulsar::core::AdapterCapabilities caps;
    caps.input_formats  = {pulsar::core::PixelFormat::BGRA, pulsar::core::PixelFormat::RGBA};
    caps.output_formats = {pulsar::core::PixelFormat::NV12};
    caps.color_spaces   = {pulsar::core::ColorSpace::BT709, pulsar::core::ColorSpace::BT2020};
    caps.supports_gpu_preprocessing = (impl_->display != nullptr);
    return caps;
}

} // namespace pulsar::preprocessor::gpu
