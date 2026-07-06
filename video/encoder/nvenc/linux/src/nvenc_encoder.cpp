// encoder/nvenc/src/nvenc_encoder.cpp
//
// NVIDIA NVENC hardware encoder — direct NVENC API via dlopen.
// Headers: encoder/nvenc/vendor/nvenc/include/nvEncodeAPI.h  [header]
//          (official nv-codec-headers, install: apt install libffmpeg-nvenc-dev)
// Runtime: libnvidia-encode.so.1  [dlopen — system GPU driver]
//          libcuda.so.1            [dlopen — system CUDA driver]

#include "nvenc_encoder.h"
#include "nvEncodeAPI.h"

#include <dlfcn.h>
#include <fcntl.h>
#include <cstring>
#include <iostream>
#include <memory>
#include <vector>

// Minimal CUDA types for context creation (no cuda.h needed at compile time).
typedef int   CUdevice;
typedef void* CUcontext;
typedef int   CUresult;
#define CUDA_SUCCESS 0
typedef CUresult (*PFN_cuInit)       (unsigned flags);
typedef CUresult (*PFN_cuDeviceGet)  (CUdevice* device, int ordinal);
typedef CUresult (*PFN_cuCtxCreate)  (CUcontext* pctx, unsigned flags, CUdevice dev);
typedef CUresult (*PFN_cuCtxDestroy) (CUcontext ctx);

namespace pulsar::encoder::nvenc {

// ─── Availability probe ───────────────────────────────────────────────────────
bool nvenc_is_available() {
    void* cuda_lib = dlopen("libcuda.so.1", RTLD_LAZY | RTLD_LOCAL);
    if (!cuda_lib) return false;
    void* nvenc_lib = dlopen("libnvidia-encode.so.1", RTLD_LAZY | RTLD_LOCAL);
    bool ok = (nvenc_lib != nullptr);
    if (nvenc_lib) dlclose(nvenc_lib);
    dlclose(cuda_lib);
    return ok;
}

// ─── Impl ─────────────────────────────────────────────────────────────────────
struct NvencEncoder::Impl {
    void* cuda_lib   = nullptr;
    void* nvenc_lib  = nullptr;

    PFN_cuInit       cuInit       = nullptr;
    PFN_cuDeviceGet  cuDeviceGet  = nullptr;
    PFN_cuCtxCreate  cuCtxCreate  = nullptr;
    PFN_cuCtxDestroy cuCtxDestroy = nullptr;

    NV_ENCODE_API_FUNCTION_LIST fn{};
    CUcontext  cu_ctx    = nullptr;
    void*      session   = nullptr;
    NV_ENC_INPUT_PTR  input_buf  = nullptr;
    NV_ENC_OUTPUT_PTR output_buf = nullptr;
    int buf_w = 0, buf_h = 0;
};

// ─── Construction ─────────────────────────────────────────────────────────────
NvencEncoder::NvencEncoder(const std::string& codec)
    : impl_(std::make_unique<Impl>())
{
    use_hevc_    = (codec == "hevc" || codec == "h265");
    initialised_ = init(codec);
    if (!initialised_) std::cerr << "[nvenc] init failed\n";
    else std::cerr << "[nvenc] ready (direct API)\n";
}

NvencEncoder::~NvencEncoder() { teardown(); }

static void* load_sym(void* lib, const char* name) { return dlsym(lib, name); }

bool NvencEncoder::init(const std::string&) {
    Impl& m = *impl_;

    // ── Load CUDA ────────────────────────────────────────────────────────────
    m.cuda_lib = dlopen("libcuda.so.1", RTLD_LAZY | RTLD_GLOBAL);
    if (!m.cuda_lib) { std::cerr<<"[nvenc] no libcuda: "<<dlerror()<<"\n"; return false; }
#define LOAD_CUDA(n) m.n = (PFN_##n)load_sym(m.cuda_lib, #n); if(!m.n){std::cerr<<"[nvenc] missing "<<#n<<"\n";return false;}
    LOAD_CUDA(cuInit) LOAD_CUDA(cuDeviceGet) LOAD_CUDA(cuCtxCreate) LOAD_CUDA(cuCtxDestroy)
#undef LOAD_CUDA
    if (m.cuInit(0) != CUDA_SUCCESS) return false;
    CUdevice dev;
    if (m.cuDeviceGet(&dev, 0) != CUDA_SUCCESS) return false;
    if (m.cuCtxCreate(&m.cu_ctx, 0, dev) != CUDA_SUCCESS) return false;

    // ── Load NVENC ───────────────────────────────────────────────────────────
    m.nvenc_lib = dlopen("libnvidia-encode.so.1", RTLD_LAZY | RTLD_GLOBAL);
    if (!m.nvenc_lib) { std::cerr<<"[nvenc] no libnvidia-encode: "<<dlerror()<<"\n"; return false; }

    // The driver supports NVENC if NvEncodeAPICreateInstance succeeds — it
    // performs the version check internally.  GetMaxSupportedVersion uses a
    // different encoding ((major<<4)|minor) than NVENCAPI_VERSION, so we
    // don't compare them here; we just let CreateInstance fail if the version
    // is truly unsupported.
    auto create = (NVENCSTATUS (NVENCAPI*)(NV_ENCODE_API_FUNCTION_LIST*))
                  load_sym(m.nvenc_lib, "NvEncodeAPICreateInstance");
    if (!create) return false;

    m.fn.version = NV_ENCODE_API_FUNCTION_LIST_VER;
    if (create(&m.fn) != NV_ENC_SUCCESS) {
        std::cerr << "[nvenc] NvEncodeAPICreateInstance failed\n"; return false;
    }

    // ── Open encode session ───────────────────────────────────────────────────
    NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS sp{};
    sp.version    = NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS_VER;
    sp.deviceType = NV_ENC_DEVICE_TYPE_CUDA;
    sp.device     = m.cu_ctx;
    sp.apiVersion = NVENCAPI_VERSION;
    if (m.fn.nvEncOpenEncodeSessionEx(&sp, &m.session) != NV_ENC_SUCCESS) {
        std::cerr << "[nvenc] OpenEncodeSessionEx failed\n"; return false;
    }
    return true;
}

void NvencEncoder::teardown() {
    if (!impl_) return;
    Impl& m = *impl_;
    if (m.session) {
        if (m.output_buf) { m.fn.nvEncDestroyBitstreamBuffer(m.session, m.output_buf); m.output_buf=nullptr; }
        if (m.input_buf)  { m.fn.nvEncDestroyInputBuffer(m.session, m.input_buf);       m.input_buf=nullptr; }
        // Drain
        NV_ENC_PIC_PARAMS eos{}; eos.version=NV_ENC_PIC_PARAMS_VER; eos.encodePicFlags=NV_ENC_PIC_FLAG_EOS;
        m.fn.nvEncEncodePicture(m.session, &eos);
        m.fn.nvEncDestroyEncoder(m.session); m.session=nullptr;
    }
    if (m.cu_ctx && m.cuCtxDestroy) { m.cuCtxDestroy(m.cu_ctx); m.cu_ctx=nullptr; }
    if (m.nvenc_lib) { dlclose(m.nvenc_lib); m.nvenc_lib=nullptr; }
    if (m.cuda_lib)  { dlclose(m.cuda_lib);  m.cuda_lib=nullptr; }
    open_ = false;
}

bool NvencEncoder::open_context(int w, int h) {
    Impl& m = *impl_;
    if (!m.session) return false;

    // Destroy old buffers
    if (m.output_buf) { m.fn.nvEncDestroyBitstreamBuffer(m.session,m.output_buf); m.output_buf=nullptr; }
    if (m.input_buf)  { m.fn.nvEncDestroyInputBuffer(m.session,m.input_buf);       m.input_buf=nullptr; }

    const GUID codec_guid   = use_hevc_ ? NV_ENC_CODEC_HEVC_GUID : NV_ENC_CODEC_H264_GUID;
    const GUID profile_guid = use_hevc_ ? NV_ENC_HEVC_PROFILE_MAIN_GUID : NV_ENC_H264_PROFILE_HIGH_GUID;

    // Get preset config
    NV_ENC_PRESET_CONFIG preset_cfg{};
    preset_cfg.version           = NV_ENC_PRESET_CONFIG_VER;
    preset_cfg.presetCfg.version = NV_ENC_CONFIG_VER;
    if (m.fn.nvEncGetEncodePresetConfigEx(m.session, codec_guid,
            NV_ENC_PRESET_P4_GUID, NV_ENC_TUNING_INFO_LOW_LATENCY, &preset_cfg) != NV_ENC_SUCCESS) {
        // Fallback to older API
        m.fn.nvEncGetEncodePresetConfig(m.session, codec_guid, NV_ENC_PRESET_P4_GUID, &preset_cfg);
    }
    NV_ENC_CONFIG& cfg = preset_cfg.presetCfg;
    cfg.profileGUID              = profile_guid;
    cfg.gopLength                = params_.gop.gop_size;
    cfg.frameIntervalP           = 1;
    cfg.rcParams.rateControlMode = NV_ENC_PARAMS_RC_CBR;
    cfg.rcParams.averageBitRate  = static_cast<uint32_t>(params_.bitrate_kbps) * 1000u;
    cfg.rcParams.maxBitRate      = static_cast<uint32_t>(params_.bitrate_kbps) * 1200u;
    // Always include SPS+PPS with every IDR so reconnecting browsers can
    // reconfigure their VideoDecoder without waiting for the next GOP start.
    if (!use_hevc_) {
        cfg.encodeCodecConfig.h264Config.repeatSPSPPS = 1;
        cfg.encodeCodecConfig.h264Config.idrPeriod    = cfg.gopLength;
    } else {
        cfg.encodeCodecConfig.hevcConfig.repeatSPSPPS = 1;
        cfg.encodeCodecConfig.hevcConfig.idrPeriod    = cfg.gopLength;
    }

    NV_ENC_INITIALIZE_PARAMS ip{};
    ip.version       = NV_ENC_INITIALIZE_PARAMS_VER;
    ip.encodeGUID    = codec_guid;
    ip.presetGUID    = NV_ENC_PRESET_P4_GUID;
    ip.encodeWidth   = static_cast<uint32_t>(w);
    ip.encodeHeight  = static_cast<uint32_t>(h);
    ip.darWidth      = static_cast<uint32_t>(w);
    ip.darHeight     = static_cast<uint32_t>(h);
    ip.frameRateNum  = static_cast<uint32_t>(params_.fps > 0 ? params_.fps : 60);
    ip.frameRateDen  = 1;
    ip.enablePTD     = 1;
    ip.tuningInfo    = NV_ENC_TUNING_INFO_LOW_LATENCY;
    ip.encodeConfig  = &cfg;
    ip.maxEncodeWidth  = 4096;
    ip.maxEncodeHeight = 4096;
    if (m.fn.nvEncInitializeEncoder(m.session, &ip) != NV_ENC_SUCCESS) {
        std::cerr<<"[nvenc] InitializeEncoder failed\n"; return false;
    }

    // Allocate I/O buffers
    NV_ENC_CREATE_INPUT_BUFFER ib{};
    ib.version   = NV_ENC_CREATE_INPUT_BUFFER_VER;
    ib.width     = static_cast<uint32_t>(w);
    ib.height    = static_cast<uint32_t>(h);
    ib.bufferFmt = NV_ENC_BUFFER_FORMAT_NV12;
    if (m.fn.nvEncCreateInputBuffer(m.session, &ib) != NV_ENC_SUCCESS) return false;
    m.input_buf = ib.inputBuffer;

    NV_ENC_CREATE_BITSTREAM_BUFFER ob{};
    ob.version = NV_ENC_CREATE_BITSTREAM_BUFFER_VER;
    if (m.fn.nvEncCreateBitstreamBuffer(m.session, &ob) != NV_ENC_SUCCESS) return false;
    m.output_buf = ob.bitstreamBuffer;

    m.buf_w = w; m.buf_h = h; open_ = true;
    return true;
}

bool NvencEncoder::encode_nv12(const uint8_t* data, int w, int h, int64_t pts_us, bool force_idr) {
    Impl& m = *impl_;
    if (!open_ || m.buf_w != w || m.buf_h != h)
        if (!open_context(w, h)) return false;

    // Lock input buffer
    NV_ENC_LOCK_INPUT_BUFFER lk{};
    lk.version     = NV_ENC_LOCK_INPUT_BUFFER_VER;
    lk.inputBuffer = m.input_buf;
    if (m.fn.nvEncLockInputBuffer(m.session, &lk) != NV_ENC_SUCCESS) return false;

    const uint32_t pitch  = lk.pitch;
    uint8_t*       dst    = static_cast<uint8_t*>(lk.bufferDataPtr);
    for (int row = 0; row < h; ++row)
        std::memcpy(dst + row * pitch, data + row * w, static_cast<size_t>(w));
    uint8_t* dst_uv = dst + static_cast<ptrdiff_t>(pitch * m.buf_h);
    const uint8_t* src_uv = data + static_cast<ptrdiff_t>(w * h);
    for (int row = 0; row < h / 2; ++row)
        std::memcpy(dst_uv + row * pitch, src_uv + row * w, static_cast<size_t>(w));

    m.fn.nvEncUnlockInputBuffer(m.session, m.input_buf);

    // Submit frame
    NV_ENC_PIC_PARAMS pp{};
    pp.version         = NV_ENC_PIC_PARAMS_VER;
    pp.inputWidth      = static_cast<uint32_t>(m.buf_w);
    pp.inputHeight     = static_cast<uint32_t>(m.buf_h);
    pp.inputPitch      = pitch;
    pp.inputBuffer     = m.input_buf;
    pp.outputBitstream = m.output_buf;
    pp.bufferFmt       = NV_ENC_BUFFER_FORMAT_NV12;
    pp.pictureStruct   = NV_ENC_PIC_STRUCT_FRAME;
    pp.inputTimeStamp  = static_cast<uint64_t>(pts_us);
    pp.encodePicFlags  = force_idr ? NV_ENC_PIC_FLAG_FORCEIDR : 0;

    NVENCSTATUS enc_rc = m.fn.nvEncEncodePicture(m.session, &pp);
    if (enc_rc != NV_ENC_SUCCESS && enc_rc != NV_ENC_ERR_NEED_MORE_INPUT) return false;

    // Retrieve output
    NV_ENC_LOCK_BITSTREAM bs{};
    bs.version         = NV_ENC_LOCK_BITSTREAM_VER;
    bs.outputBitstream = m.output_buf;
    if (m.fn.nvEncLockBitstream(m.session, &bs) != NV_ENC_SUCCESS) return false;

    if (bs.bitstreamSizeInBytes > 0 && callback_) {
        struct Pkt final : public pulsar::core::PacketBuffer {
            std::vector<uint8_t> d_;
            Pkt(const void* s, size_t n) : d_(static_cast<const uint8_t*>(s), static_cast<const uint8_t*>(s)+n) {}
            const uint8_t* data() const override { return d_.data(); }
            size_t         size() const override { return d_.size(); }
        };
        pulsar::core::EncodedPacket pkt;
        pkt.buffer      = std::make_shared<Pkt>(bs.bitstreamBufferPtr, bs.bitstreamSizeInBytes);
        pkt.is_keyframe = (bs.pictureType == NV_ENC_PIC_TYPE_IDR);
        pkt.pts_us      = static_cast<int64_t>(bs.outputTimeStamp);
        pkt.codec       = use_hevc_ ? pulsar::core::CodecType::H265 : pulsar::core::CodecType::H264;
        callback_(std::move(pkt));
    }
    m.fn.nvEncUnlockBitstream(m.session, m.output_buf);
    return true;
}

void NvencEncoder::submit_frame(pulsar::core::RawFrame frame, pulsar::core::SubmitFlags flags) {
    if (!initialised_ || !callback_) return;
    if (!frame.buffer || frame.width<=0 || frame.height<=0) return;
    if (frame.format != pulsar::core::PixelFormat::NV12) return;
    encode_nv12(frame.buffer->data(), frame.width, frame.height,
                frame.pts_us, flags == pulsar::core::SubmitFlags::ForceKeyframe);
}

void NvencEncoder::set_encoded_callback(std::function<void(pulsar::core::EncodedPacket)> cb) { callback_=std::move(cb); }
void NvencEncoder::update_params(const pulsar::core::EncoderParams& p) { params_=p; if(open_){teardown();impl_=std::make_unique<Impl>();} }

pulsar::core::AdapterCapabilities NvencEncoder::capabilities() const {
    pulsar::core::AdapterCapabilities caps;
    caps.input_formats         = {pulsar::core::PixelFormat::NV12};
    caps.output_formats        = {pulsar::core::PixelFormat::NV12};
    caps.color_spaces          = {pulsar::core::ColorSpace::BT709, pulsar::core::ColorSpace::BT2020};
    caps.supports_async_encode = initialised_;
    caps.supports_dmabuf       = false;
    return caps;
}

} // namespace pulsar::encoder::nvenc
