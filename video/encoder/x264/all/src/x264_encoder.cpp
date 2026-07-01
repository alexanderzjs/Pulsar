// encoder/x264/src/x264_encoder.cpp
// Software H.264 encoder using the vendored libx264 directly.
// Dependency: encoder/x264/vendor/x264/lib/linux_x86_64/libx264.a  [static]
//             encoder/x264/vendor/x264/include/x264.h               [header]

#include "x264_encoder.h"
#include "x264.h"

#include <cstring>
#include <iostream>
#include <memory>
#include <vector>

namespace pulsar::encoder::x264 {

struct X264Encoder::Impl {
    x264_t*           enc    = nullptr;
    x264_picture_t    pic_in{};
    x264_picture_t    pic_out{};
    x264_param_t      param{};
    int               width  = 0;
    int               height = 0;
    int               frame_num = 0;
};

X264Encoder::X264Encoder() : impl_(std::make_unique<Impl>()) {}
X264Encoder::~X264Encoder() { close(); }

void X264Encoder::close() {
    if (!impl_) return;
    if (impl_->pic_in.img.plane[0])
        x264_picture_clean(&impl_->pic_in);
    if (impl_->enc) {
        x264_encoder_close(impl_->enc);
        impl_->enc = nullptr;
    }
    open_ = false;
}

bool X264Encoder::open_context(int width, int height) {
    close();
    impl_ = std::make_unique<Impl>();
    Impl& m = *impl_;

    // ── Encoder params ────────────────────────────────────────────────────
    x264_param_default_preset(&m.param, "veryfast", "zerolatency");
    m.param.i_width          = width;
    m.param.i_height         = height;
    m.param.i_fps_num        = static_cast<uint32_t>(params_.fps > 0 ? params_.fps : 60);
    m.param.i_fps_den        = 1;
    m.param.i_keyint_max     = params_.gop.gop_size > 0 ? params_.gop.gop_size : 120;
    m.param.i_keyint_min     = m.param.i_keyint_max / 2;
    m.param.i_bframe         = params_.gop.max_b_frames;
    m.param.rc.i_rc_method   = X264_RC_ABR;
    m.param.rc.i_bitrate     = params_.bitrate_kbps > 0 ? params_.bitrate_kbps : 4000;
    m.param.i_log_level      = X264_LOG_ERROR;
    m.param.b_repeat_headers = 1;   // include SPS/PPS in every keyframe NAL
    m.param.b_annexb          = 1;   // Annex-B start codes

    x264_param_apply_profile(&m.param, "high");

    // ── Open encoder ──────────────────────────────────────────────────────
    m.enc = x264_encoder_open(&m.param);
    if (!m.enc) {
        std::cerr << "[x264] x264_encoder_open failed\n";
        return false;
    }

    // ── Allocate input picture ────────────────────────────────────────────
    x264_picture_init(&m.pic_in);
    if (x264_picture_alloc(&m.pic_in, X264_CSP_NV12, width, height) < 0) {
        x264_encoder_close(m.enc);
        m.enc = nullptr;
        return false;
    }

    m.width     = width;
    m.height    = height;
    m.frame_num = 0;
    open_       = true;
    return true;
}

void X264Encoder::submit_frame(pulsar::core::RawFrame frame,
                                pulsar::core::SubmitFlags flags)
{
    if (!callback_ || !frame.buffer || frame.width <= 0 || frame.height <= 0) return;
    if (frame.format != pulsar::core::PixelFormat::NV12) return;

    const int w = frame.width, h = frame.height;
    if (!open_ || impl_->width != w || impl_->height != h)
        if (!open_context(w, h)) return;

    Impl& m = *impl_;

    // ── Copy NV12 data into x264 picture ─────────────────────────────────
    // x264 NV12: plane[0] = Y, plane[1] = interleaved UV
    const int y_size = w * h;
    std::memcpy(m.pic_in.img.plane[0], frame.buffer->data(),
                static_cast<size_t>(y_size));
    std::memcpy(m.pic_in.img.plane[1], frame.buffer->data() + y_size,
                static_cast<size_t>(y_size / 2));

    m.pic_in.i_pts  = m.frame_num++;
    m.pic_in.i_type = (flags == pulsar::core::SubmitFlags::ForceKeyframe)
                      ? X264_TYPE_IDR : X264_TYPE_AUTO;

    // ── Encode ─────────────────────────────────────────────────────────────
    x264_nal_t*  nals     = nullptr;
    int          nal_count = 0;
    int          frame_size = x264_encoder_encode(m.enc, &nals, &nal_count,
                                                   &m.pic_in, &m.pic_out);
    if (frame_size < 0 || nal_count == 0) return;

    // Concatenate all NAL units into a single buffer.
    size_t total = 0;
    for (int i = 0; i < nal_count; ++i)
        total += static_cast<size_t>(nals[i].i_payload);
    if (total == 0) return;

    struct NalBuffer final : public pulsar::core::PacketBuffer {
        std::vector<uint8_t> d_;
        NalBuffer() {}
        const uint8_t* data() const override { return d_.data(); }
        size_t         size() const override { return d_.size(); }
    };
    auto buf = std::make_shared<NalBuffer>();
    buf->d_.reserve(total);
    for (int i = 0; i < nal_count; ++i)
        buf->d_.insert(buf->d_.end(),
                       nals[i].p_payload,
                       nals[i].p_payload + nals[i].i_payload);

    pulsar::core::EncodedPacket pkt;
    pkt.buffer      = std::move(buf);
    pkt.is_keyframe = (m.pic_out.i_type == X264_TYPE_IDR ||
                       m.pic_out.i_type == X264_TYPE_I);
    pkt.pts_us      = frame.pts_us;
    pkt.codec       = pulsar::core::CodecType::H264;
    callback_(std::move(pkt));
}

void X264Encoder::set_encoded_callback(
    std::function<void(pulsar::core::EncodedPacket)> cb) {
    callback_ = std::move(cb);
}

void X264Encoder::update_params(const pulsar::core::EncoderParams& params) {
    params_ = params;
    if (open_) close();
}

pulsar::core::AdapterCapabilities X264Encoder::capabilities() const {
    pulsar::core::AdapterCapabilities caps;
    caps.input_formats         = {pulsar::core::PixelFormat::NV12};
    caps.output_formats        = {pulsar::core::PixelFormat::NV12};
    caps.color_spaces          = {pulsar::core::ColorSpace::BT709};
    caps.supports_async_encode = false;
    caps.supports_dmabuf       = false;
    return caps;
}

} // namespace pulsar::encoder::x264
