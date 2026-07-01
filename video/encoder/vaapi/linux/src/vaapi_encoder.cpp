// encoder/vaapi/src/vaapi_encoder.cpp
// Full VAAPI H.264 hardware encoder.
// Dependencies: libva / libva-drm  [system: pkg-config]
//
// Pipeline per frame:
//   vaBeginPicture → vaRenderPicture(seq + pic + slice params) → vaEndPicture
//   → vaSyncSurface → vaMapBuffer(coded_buf) → copy bitstream → vaUnmapBuffer

#include "vaapi_encoder.h"

#include <fcntl.h>
#include <unistd.h>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <vector>

#include <va/va.h>
#include <va/va_drm.h>
#include <va/va_enc_h264.h>

namespace pulsar::encoder::vaapi {

// ─── helpers ─────────────────────────────────────────────────────────────────
namespace {

static bool try_open_va(VADisplay& out_display, int& out_fd) {
    out_fd = ::open("/dev/dri/renderD128", O_RDWR | O_CLOEXEC);
    if (out_fd < 0) return false;
    out_display = vaGetDisplayDRM(out_fd);
    if (!out_display) { ::close(out_fd); out_fd=-1; return false; }
    static const char* kDrivers[] = {"nvidia","iHD","i965","radeonsi","nouveau",nullptr};
    const char* prev = std::getenv("LIBVA_DRIVER_NAME");
    const std::string prev_str = prev ? prev : "";
    int maj=0,min_=0; bool ok=false;
    for (const char** d=kDrivers; *d; ++d) {
        ::setenv("LIBVA_DRIVER_NAME",*d,1);
        if (vaInitialize(out_display,&maj,&min_)==VA_STATUS_SUCCESS){ok=true;break;}
    }
    if (prev) ::setenv("LIBVA_DRIVER_NAME",prev_str.c_str(),1);
    else      ::unsetenv("LIBVA_DRIVER_NAME");
    if (!ok) { ::close(out_fd); out_fd=-1; }
    return ok;
}

struct HeapPacket final : public pulsar::core::PacketBuffer {
    std::vector<uint8_t> d_;
    HeapPacket(const void* s, size_t n)
        : d_(static_cast<const uint8_t*>(s),static_cast<const uint8_t*>(s)+n) {}
    const uint8_t* data() const override { return d_.data(); }
    size_t         size() const override { return d_.size(); }
};

} // namespace

// ─── Impl ─────────────────────────────────────────────────────────────────────
struct VaapiEncoder::Impl {
    VADisplay   display     = nullptr;
    int         drm_fd      = -1;
    VAConfigID  config_id   = VA_INVALID_ID;
    VAContextID context_id  = VA_INVALID_ID;
    VASurfaceID input_surf  = VA_INVALID_SURFACE;
    VABufferID  coded_buf   = VA_INVALID_ID;
    int         width       = 0;
    int         height      = 0;
    int         frame_num   = 0;
};

// ─── availability probe ───────────────────────────────────────────────────────
bool vaapi_is_available() {
    VADisplay d{}; int fd = -1;
    if (!try_open_va(d, fd)) return false;

    // Verify that VAEntrypointEncSlice is supported for VAProfileH264High.
    // Some platforms open a display but don't have encode capability.
    int num_profiles = vaMaxNumProfiles(d);
    std::vector<VAProfile> profiles(static_cast<size_t>(num_profiles));
    int actual = 0;
    VAStatus st = vaQueryConfigProfiles(d, profiles.data(), &actual);
    if (st != VA_STATUS_SUCCESS) { vaTerminate(d); ::close(fd); return false; }

    bool h264_found = false;
    for (int i = 0; i < actual; ++i) {
        if (profiles[static_cast<size_t>(i)] == VAProfileH264High) {
            h264_found = true; break;
        }
    }

    if (h264_found) {
        int num_entrypoints = vaMaxNumEntrypoints(d);
        std::vector<VAEntrypoint> eps(static_cast<size_t>(num_entrypoints));
        int ep_count = 0;
        st = vaQueryConfigEntrypoints(d, VAProfileH264High, eps.data(), &ep_count);
        if (st != VA_STATUS_SUCCESS) h264_found = false;
        else {
            bool enc_found = false;
            for (int i = 0; i < ep_count; ++i) {
                if (eps[static_cast<size_t>(i)] == VAEntrypointEncSlice) {
                    enc_found = true; break;
                }
            }
            h264_found = enc_found;
        }
    }

    vaTerminate(d); ::close(fd);
    return h264_found;
}

// ─── Construction ─────────────────────────────────────────────────────────────
VaapiEncoder::VaapiEncoder(const std::string& codec)
    : impl_(std::make_unique<Impl>())
{
    use_hevc_    = (codec=="hevc"||codec=="h265");
    if (use_hevc_) {
        std::cerr << "[vaapi] HEVC not yet supported in Phase 2; falling back\n";
        initialised_ = false;
        return;
    }
    initialised_ = try_open_va(impl_->display, impl_->drm_fd);
    if (!initialised_) std::cerr << "[vaapi] display not available\n";
    else               std::cerr << "[vaapi] display opened\n";
}

VaapiEncoder::~VaapiEncoder() {
    teardown();
    if (impl_->display) { vaTerminate(impl_->display); impl_->display=nullptr; }
    if (impl_->drm_fd>=0) { ::close(impl_->drm_fd); impl_->drm_fd=-1; }
}

void VaapiEncoder::teardown() {
    if (!impl_||!impl_->display) return;
    if (impl_->coded_buf   !=VA_INVALID_ID)     { vaDestroyBuffer(impl_->display,impl_->coded_buf);    impl_->coded_buf=VA_INVALID_ID; }
    if (impl_->input_surf  !=VA_INVALID_SURFACE){ vaDestroySurfaces(impl_->display,&impl_->input_surf,1); impl_->input_surf=VA_INVALID_SURFACE; }
    if (impl_->context_id  !=VA_INVALID_ID)     { vaDestroyContext(impl_->display,impl_->context_id);  impl_->context_id=VA_INVALID_ID; }
    if (impl_->config_id   !=VA_INVALID_ID)     { vaDestroyConfig(impl_->display,impl_->config_id);    impl_->config_id=VA_INVALID_ID; }
    open_=false;
}

bool VaapiEncoder::open_context(int w, int h) {
    teardown();
    Impl& m = *impl_;

    // Config: H.264 High profile, encode entrypoint
    VAConfigAttrib attr[2];
    attr[0].type=VAConfigAttribRTFormat;     attr[0].value=VA_RT_FORMAT_YUV420;
    attr[1].type=VAConfigAttribEncMaxRefFrames; attr[1].value=1;
    if (vaCreateConfig(m.display, VAProfileH264High,
                       VAEntrypointEncSlice, attr, 2, &m.config_id) != VA_STATUS_SUCCESS) {
        std::cerr<<"[vaapi] vaCreateConfig failed\n"; return false;
    }

    // Input surface (NV12)
    VASurfaceAttrib surf_attr{}; surf_attr.type=VASurfaceAttribPixelFormat; surf_attr.flags=VA_SURFACE_ATTRIB_SETTABLE; surf_attr.value.type=VAGenericValueTypeInteger; surf_attr.value.value.i=VA_FOURCC_NV12;
    if (vaCreateSurfaces(m.display, VA_RT_FORMAT_YUV420,
                         static_cast<unsigned>(w), static_cast<unsigned>(h),
                         &m.input_surf, 1, &surf_attr, 1) != VA_STATUS_SUCCESS) {
        std::cerr<<"[vaapi] vaCreateSurfaces failed\n"; teardown(); return false;
    }

    // Encode context
    if (vaCreateContext(m.display, m.config_id, w, h,
                        VA_PROGRESSIVE, &m.input_surf, 1,
                        &m.context_id) != VA_STATUS_SUCCESS) {
        std::cerr<<"[vaapi] vaCreateContext failed\n"; teardown(); return false;
    }

    // Coded output buffer (4× frame size is a safe upper bound)
    if (vaCreateBuffer(m.display, m.context_id, VAEncCodedBufferType,
                       static_cast<unsigned>(w * h * 4), 1, nullptr,
                       &m.coded_buf) != VA_STATUS_SUCCESS) {
        std::cerr<<"[vaapi] vaCreateBuffer(coded) failed\n"; teardown(); return false;
    }

    m.width=w; m.height=h; m.frame_num=0; open_=true;
    return true;
}

bool VaapiEncoder::encode_nv12(const uint8_t* data, int w, int h,
                                int64_t pts_us, bool force_idr) {
    if (!open_||impl_->width!=w||impl_->height!=h)
        if (!open_context(w,h)) return false;
    Impl& m = *impl_;

    // Upload NV12 pixels to the VASurface via VAImage
    VAImage img{}; void* buf_ptr=nullptr;
    if (vaDeriveImage(m.display, m.input_surf, &img) == VA_STATUS_SUCCESS) {
        vaMapBuffer(m.display, img.buf, &buf_ptr);
        auto* dst = static_cast<uint8_t*>(buf_ptr);
        // Y plane
        for (int row=0; row<h; ++row)
            std::memcpy(dst + img.offsets[0] + row*static_cast<int>(img.pitches[0]),
                        data + row*w, static_cast<size_t>(w));
        // UV plane
        for (int row=0; row<h/2; ++row)
            std::memcpy(dst + img.offsets[1] + row*static_cast<int>(img.pitches[1]),
                        data + w*h + row*w, static_cast<size_t>(w));
        vaUnmapBuffer(m.display, img.buf);
        vaDestroyImage(m.display, img.image_id);
    } else {
        // vaDeriveImage not supported: use vaPutImage fallback
        VAImageFormat fmt{}; fmt.fourcc=VA_FOURCC_NV12;
        fmt.byte_order=VA_LSB_FIRST; fmt.bits_per_pixel=12;
        if (vaCreateImage(m.display,&fmt,w,h,&img)!=VA_STATUS_SUCCESS) return false;
        vaMapBuffer(m.display,img.buf,&buf_ptr);
        auto* dst=static_cast<uint8_t*>(buf_ptr);
        for (int row=0;row<h;++row)
            std::memcpy(dst+img.offsets[0]+row*static_cast<int>(img.pitches[0]),data+row*w,static_cast<size_t>(w));
        for (int row=0;row<h/2;++row)
            std::memcpy(dst+img.offsets[1]+row*static_cast<int>(img.pitches[1]),data+w*h+row*w,static_cast<size_t>(w));
        vaUnmapBuffer(m.display,img.buf);
        vaPutImage(m.display,m.input_surf,img.image_id,0,0,static_cast<unsigned>(w),static_cast<unsigned>(h),0,0,static_cast<unsigned>(w),static_cast<unsigned>(h));
        vaDestroyImage(m.display,img.image_id);
    }

    // ── Sequence parameter buffer ─────────────────────────────────────────────
    bool is_idr = force_idr || (m.frame_num == 0);
    VAEncSequenceParameterBufferH264 seq{};
    seq.seq_parameter_set_id     = 0;
    seq.level_idc                = 41;
    seq.intra_period             = static_cast<unsigned>(params_.gop.gop_size);
    seq.ip_period                = 1;
    seq.bits_per_second          = static_cast<unsigned>(params_.bitrate_kbps) * 1000u;
    seq.max_num_ref_frames       = 1;
    seq.picture_width_in_mbs     = static_cast<unsigned>((w+15)/16);
    seq.picture_height_in_mbs    = static_cast<unsigned>((h+15)/16);
    seq.frame_cropping_flag      = (seq.picture_width_in_mbs*16!=static_cast<unsigned>(w)||seq.picture_height_in_mbs*16!=static_cast<unsigned>(h));
    seq.frame_crop_right_offset  = (seq.picture_width_in_mbs*16-static_cast<unsigned>(w))/2;
    seq.frame_crop_bottom_offset = (seq.picture_height_in_mbs*16-static_cast<unsigned>(h))/2;
    VABufferID seq_buf=VA_INVALID_ID;
    vaCreateBuffer(m.display,m.context_id,VAEncSequenceParameterBufferType,sizeof(seq),1,&seq,&seq_buf);

    // ── Picture parameter buffer ───────────────────────────────────────────────
    VAEncPictureParameterBufferH264 pic{};
    pic.CurrPic.picture_id         = m.input_surf;
    pic.CurrPic.frame_idx          = static_cast<unsigned>(m.frame_num);
    pic.CurrPic.flags              = is_idr ? H264_LAST_PICTURE_EOSEQ|H264_LAST_PICTURE_EOSTREAM : 0;
    pic.CurrPic.TopFieldOrderCnt   = m.frame_num * 2;
    pic.CurrPic.BottomFieldOrderCnt= m.frame_num * 2;
    for (auto& r : pic.ReferenceFrames) r.picture_id=VA_INVALID_SURFACE;
    pic.coded_buf                  = m.coded_buf;
    pic.pic_parameter_set_id       = 0;
    pic.pic_init_qp                = 26;
    pic.num_ref_idx_l0_active_minus1 = 0;
    pic.num_ref_idx_l1_active_minus1 = 0;
    pic.pic_fields.bits.idr_pic_flag              = is_idr ? 1 : 0;
    pic.pic_fields.bits.reference_pic_flag        = 1;
    pic.pic_fields.bits.entropy_coding_mode_flag  = 1; // CABAC
    pic.pic_fields.bits.weighted_pred_flag        = 0;
    pic.pic_fields.bits.constrained_intra_pred_flag=0;
    pic.pic_fields.bits.transform_8x8_mode_flag   = 1;
    VABufferID pic_buf=VA_INVALID_ID;
    vaCreateBuffer(m.display,m.context_id,VAEncPictureParameterBufferType,sizeof(pic),1,&pic,&pic_buf);

    // ── Slice parameter buffer ─────────────────────────────────────────────────
    VAEncSliceParameterBufferH264 slice{};
    slice.macroblock_address             = 0;
    slice.num_macroblocks                = seq.picture_width_in_mbs * seq.picture_height_in_mbs;
    slice.slice_type                     = is_idr ? 2 : 0; // I / P
    slice.pic_parameter_set_id          = 0;
    slice.cabac_init_idc                 = 0;
    slice.slice_qp_delta                 = 0;
    slice.disable_deblocking_filter_idc  = 0;
    for (auto& r : slice.RefPicList0) r.picture_id=VA_INVALID_SURFACE;
    for (auto& r : slice.RefPicList1) r.picture_id=VA_INVALID_SURFACE;
    VABufferID slice_buf=VA_INVALID_ID;
    vaCreateBuffer(m.display,m.context_id,VAEncSliceParameterBufferType,sizeof(slice),1,&slice,&slice_buf);

    // ── Render + sync ─────────────────────────────────────────────────────────
    vaBeginPicture(m.display, m.context_id, m.input_surf);
    VABufferID bufs[] = {seq_buf, pic_buf, slice_buf};
    vaRenderPicture(m.display, m.context_id, bufs, 3);
    vaEndPicture(m.display, m.context_id);
    vaSyncSurface(m.display, m.input_surf);

    // ── Read coded bitstream ───────────────────────────────────────────────────
    VACodedBufferSegment* seg = nullptr;
    if (vaMapBuffer(m.display, m.coded_buf,
                    reinterpret_cast<void**>(&seg)) == VA_STATUS_SUCCESS) {
        std::vector<uint8_t> out_data;
        while (seg) {
            if (seg->buf && seg->size > 0)
                out_data.insert(out_data.end(),
                                static_cast<uint8_t*>(seg->buf),
                                static_cast<uint8_t*>(seg->buf) + seg->size);
            seg = reinterpret_cast<VACodedBufferSegment*>(seg->next);
        }
        vaUnmapBuffer(m.display, m.coded_buf);

        if (!out_data.empty() && callback_) {
            pulsar::core::EncodedPacket pkt;
            pkt.buffer      = std::make_shared<HeapPacket>(out_data.data(), out_data.size());
            pkt.is_keyframe = is_idr;
            pkt.pts_us      = pts_us;
            pkt.codec       = pulsar::core::CodecType::H264;
            callback_(std::move(pkt));
        }
    }

    // Clean up per-frame buffers
    vaDestroyBuffer(m.display, seq_buf);
    vaDestroyBuffer(m.display, pic_buf);
    vaDestroyBuffer(m.display, slice_buf);
    ++m.frame_num;
    return true;
}

// ─── IEncoder interface ───────────────────────────────────────────────────────
void VaapiEncoder::submit_frame(pulsar::core::RawFrame frame,
                                pulsar::core::SubmitFlags flags) {
    if (!initialised_||!callback_) return;
    if (!frame.buffer||frame.width<=0||frame.height<=0) return;
    if (frame.format!=pulsar::core::PixelFormat::NV12) return;
    encode_nv12(frame.buffer->data(),frame.width,frame.height,frame.pts_us,
                flags==pulsar::core::SubmitFlags::ForceKeyframe);
}
void VaapiEncoder::set_encoded_callback(std::function<void(pulsar::core::EncodedPacket)> cb){callback_=std::move(cb);}
void VaapiEncoder::update_params(const pulsar::core::EncoderParams& p){params_=p;if(open_)teardown();}
pulsar::core::AdapterCapabilities VaapiEncoder::capabilities() const {
    pulsar::core::AdapterCapabilities caps;
    caps.input_formats={pulsar::core::PixelFormat::NV12};
    caps.output_formats={pulsar::core::PixelFormat::NV12};
    caps.color_spaces={pulsar::core::ColorSpace::BT709,pulsar::core::ColorSpace::BT2020};
    caps.supports_async_encode=initialised_;
    caps.supports_dmabuf=initialised_;
    return caps;
}

} // namespace pulsar::encoder::vaapi
