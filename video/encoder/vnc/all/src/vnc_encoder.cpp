// encoder/vnc/src/vnc_encoder.cpp
// VNC Encoder: packages raw BGRA frames as RFB FramebufferUpdate messages.
//
// RFB wire format produced (RFC 6143 §7.6.1):
//
//   ┌───────────────────────────────────────────────────────┐
//   │  msg-type       uint8  = 0 (FramebufferUpdate)        │
//   │  padding        uint8  = 0                            │
//   │  number-of-rects uint16  (big-endian)                 │
//   │  for each rect:                                       │
//   │    x-position   uint16 BE                             │
//   │    y-position   uint16 BE                             │
//   │    width        uint16 BE                             │
//   │    height       uint16 BE                             │
//   │    encoding     int32  BE  = 0 (Raw)                  │
//   │    pixel-data   width×height×4 bytes (BGRA, top-down) │
//   └───────────────────────────────────────────────────────┘
//
// Pixel format: 32 bpp BGRA (blue=first byte, alpha=last).  This matches what
// VncTransport negotiates during the ServerInit / SetPixelFormat exchange.
//
// Dirty-rect awareness: only changed rectangles are encoded per frame.
// If no dirty rects are provided, a single full-frame rectangle is used.

#include "vnc_encoder.h"

#include <algorithm>
#include <cstring>
#include <vector>

namespace pulsar::encoder::vnc {
// ─── NV12 → BGRA conversion (BT.601 full-range) ─────────────────────────────

static std::vector<uint8_t> nv12_to_bgra(const uint8_t* src, int W, int H) {
    const uint8_t* Y  = src;
    const uint8_t* UV = src + static_cast<size_t>(W * H);
    std::vector<uint8_t> out(static_cast<size_t>(W * H * 4));
    uint8_t* dst = out.data();
    for (int row = 0; row < H; ++row) {
        const uint8_t* uv_row = UV + static_cast<size_t>((row >> 1) * W);
        for (int col = 0; col < W; ++col) {
            const int y  = static_cast<int>(Y[row * W + col]);
            const int u  = static_cast<int>(uv_row[col & ~1])       - 128;
            const int v  = static_cast<int>(uv_row[(col & ~1) | 1]) - 128;
            const int r  = std::clamp(y + 1402 * v / 1000,                  0, 255);
            const int g  = std::clamp(y - 344  * u / 1000 - 714 * v / 1000, 0, 255);
            const int b  = std::clamp(y + 1772 * u / 1000,                  0, 255);
            *dst++ = static_cast<uint8_t>(b);
            *dst++ = static_cast<uint8_t>(g);
            *dst++ = static_cast<uint8_t>(r);
            *dst++ = 0xFF;
        }
    }
    return out;
}
// ─── Helpers ─────────────────────────────────────────────────────────────────

static void write_be16(std::vector<uint8_t>& v, uint16_t x) {
    v.push_back(static_cast<uint8_t>(x >> 8));
    v.push_back(static_cast<uint8_t>(x));
}
static void write_be32(std::vector<uint8_t>& v, uint32_t x) {
    v.push_back(static_cast<uint8_t>(x >> 24));
    v.push_back(static_cast<uint8_t>(x >> 16));
    v.push_back(static_cast<uint8_t>(x >> 8));
    v.push_back(static_cast<uint8_t>(x));
}

// ─── Impl ─────────────────────────────────────────────────────────────────────

struct VncEncoder::Impl {};

VncEncoder::VncEncoder() : impl_(std::make_unique<Impl>()) {}
VncEncoder::~VncEncoder() = default;

void VncEncoder::update_params(const pulsar::core::EncoderParams& /*p*/) {
    // VNC raw encoding has no bitrate/quality parameters.
}

pulsar::core::AdapterCapabilities VncEncoder::capabilities() const {
    pulsar::core::AdapterCapabilities c;
    // VNC directly uses BGRA; also accepts NV12 (converted internally).
    c.input_formats  = { pulsar::core::PixelFormat::BGRA,
                         pulsar::core::PixelFormat::RGBA,
                         pulsar::core::PixelFormat::NV12 };
    c.output_formats = {};
    c.supports_dirty_rect = true;
    return c;
}

void VncEncoder::set_encoded_callback(
        std::function<void(pulsar::core::EncodedPacket)> cb) {
    callback_ = std::move(cb);
}

// ─── submit_frame ─────────────────────────────────────────────────────────────
//
// Builds a RFB FramebufferUpdate for the given set of dirty rectangles.
// Pixel data is BGRA, top-down row order (matching RFB Raw encoding).

void VncEncoder::submit_frame(pulsar::core::RawFrame frame,
                               pulsar::core::SubmitFlags /*flags*/) {
    if (!callback_ || !frame.buffer) return;

    const int W = frame.width;
    const int H = frame.height;
    if (W <= 0 || H <= 0) return;

    const uint8_t* src = frame.buffer->data();
    if (!src) return;

    // If input is NV12, convert to BGRA before building RFB message.
    std::vector<uint8_t> bgra_tmp;
    if (frame.format == pulsar::core::PixelFormat::NV12) {
        bgra_tmp = nv12_to_bgra(src, W, H);
        src = bgra_tmp.data();
    }

    std::vector<pulsar::core::DirtyRect> rects = frame.dirty_rects;
    if (rects.empty()) rects.push_back({0, 0, W, H});

    // Clamp rectangles to frame bounds.
    std::vector<pulsar::core::DirtyRect> valid;
    valid.reserve(rects.size());
    for (auto& r : rects) {
        const int rx = std::max(0, r.x);
        const int ry = std::max(0, r.y);
        const int rw = std::min(r.width,  W - rx);
        const int rh = std::min(r.height, H - ry);
        if (rw > 0 && rh > 0) valid.push_back({rx, ry, rw, rh});
    }
    if (valid.empty()) return;

    const int kBpp = 4; // 32-bit BGRA
    const size_t src_stride = static_cast<size_t>(W * kBpp);

    // Build RFB FramebufferUpdate message.
    std::vector<uint8_t> msg;
    msg.reserve(4 + valid.size() * (12 + static_cast<size_t>(W * H * kBpp)));

    msg.push_back(0);   // msg-type = FramebufferUpdate
    msg.push_back(0);   // padding
    write_be16(msg, static_cast<uint16_t>(valid.size())); // number-of-rects

    for (const auto& r : valid) {
        write_be16(msg, static_cast<uint16_t>(r.x));
        write_be16(msg, static_cast<uint16_t>(r.y));
        write_be16(msg, static_cast<uint16_t>(r.width));
        write_be16(msg, static_cast<uint16_t>(r.height));
        write_be32(msg, 0); // encoding = Raw (0)

        // Pixel data: top-down, row by row.
        for (int row = 0; row < r.height; ++row) {
            const uint8_t* row_ptr = src
                + static_cast<size_t>(r.y + row) * src_stride
                + static_cast<size_t>(r.x) * kBpp;
            msg.insert(msg.end(), row_ptr,
                       row_ptr + static_cast<size_t>(r.width * kBpp));
        }
    }

    struct VncPacketBuffer final : public pulsar::core::PacketBuffer {
        std::vector<uint8_t> d_;
        explicit VncPacketBuffer(std::vector<uint8_t> d) : d_(std::move(d)) {}
        const uint8_t* data() const override { return d_.data(); }
        size_t         size() const override { return d_.size(); }
    };

    pulsar::core::EncodedPacket pkt;
    pkt.buffer      = std::make_shared<VncPacketBuffer>(std::move(msg));
    pkt.is_keyframe = true;
    pkt.pts_us      = frame.pts_us;
    pkt.codec       = pulsar::core::CodecType::H264; // sentinel
    callback_(std::move(pkt));
}

} // namespace pulsar::encoder::vnc
