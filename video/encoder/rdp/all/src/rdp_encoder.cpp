// encoder/rdp/src/rdp_encoder.cpp
// RDP Encoder: packages raw BGRA frames as an RDP Bitmap Cache Offer /
// Uncompressed Bitmap Update PDU, ready to be sent by RdpTransport.
//
// Wire format produced (per frame):
//
//   ┌─────────────────────────────────────────────────────┐
//   │  RDP PDU header (see rdp_pdu_t below)               │
//   │  TS_FP_UPDATE_PDU / UpdateType=BITMAP               │
//   │  TS_UPDATE_BITMAP_DATA                              │
//   │  TS_BITMAP_DATA[0..n] (one rectangle per dirty_rect)│
//   │    bitmapDataStream: raw BGRA scanlines (bottom-up) │
//   └─────────────────────────────────────────────────────┘
//
// The PDU is wrapped in an EncodedPacket whose buffer holds the raw bytes;
// RdpTransport::send() writes them verbatim to the TCP socket (it owns
// the X.224/MCS/T.128 framing layer above).
//
// RDP spec references:
//   MS-RDPBCGR §2.2.9.1.1.3.1  TS_BITMAP_DATA
//   MS-RDPBCGR §2.2.9.1.2      Fast-Path Bitmap Update

#include "rdp_encoder.h"

#include <algorithm>
#include <cstring>
#include <iostream>
#include <vector>

namespace pulsar::encoder::rdp {

// ─── Wire layout helpers ─────────────────────────────────────────────────────

// TS_BITMAP_DATA flags (MS-RDPBCGR §2.2.9.1.1.3.1.1)
static constexpr uint16_t BITMAP_COMPRESSION = 0x0001;
static constexpr uint16_t NO_BITMAP_COMPRESSION_HDR = 0x0400;

// TS_FP_UPDATE header bits (Fast-Path)
static constexpr uint8_t FASTPATH_UPDATE_BITMAP = 0x01;
static constexpr uint8_t FASTPATH_FRAGMENT_SINGLE = 0x30; // updateHeader bits [7:4]

static void write_le16(std::vector<uint8_t>& v, uint16_t x) {
    v.push_back(static_cast<uint8_t>(x));
    v.push_back(static_cast<uint8_t>(x >> 8));
}
static void write_le32(std::vector<uint8_t>& v, uint32_t x) {
    v.push_back(static_cast<uint8_t>(x));
    v.push_back(static_cast<uint8_t>(x >> 8));
    v.push_back(static_cast<uint8_t>(x >> 16));
    v.push_back(static_cast<uint8_t>(x >> 24));
}

// ─── NV12 → BGRA conversion (BT.601 full-range) ─────────────────────────────
//
// NV12 layout: [Y plane: W×H bytes][UV interleaved: W×(H/2) bytes]
// Output: BGRA 32 bpp, top-down, 4 bytes/pixel.

static std::vector<uint8_t> nv12_to_bgra(const uint8_t* src, int W, int H) {
    const uint8_t* Y  = src;
    const uint8_t* UV = src + static_cast<size_t>(W * H);
    std::vector<uint8_t> out(static_cast<size_t>(W * H * 4));
    uint8_t* dst = out.data();
    for (int row = 0; row < H; ++row) {
        const uint8_t* uv_row = UV + static_cast<size_t>((row >> 1) * W);
        for (int col = 0; col < W; ++col) {
            const int y  = static_cast<int>(Y[row * W + col]);
            const int u  = static_cast<int>(uv_row[col & ~1])     - 128;
            const int v  = static_cast<int>(uv_row[col & ~1 | 1]) - 128;
            const int r  = std::clamp(y + 1402 * v / 1000,             0, 255);
            const int g  = std::clamp(y - 344  * u / 1000 - 714 * v / 1000, 0, 255);
            const int b  = std::clamp(y + 1772 * u / 1000,             0, 255);
            *dst++ = static_cast<uint8_t>(b);
            *dst++ = static_cast<uint8_t>(g);
            *dst++ = static_cast<uint8_t>(r);
            *dst++ = 0xFF; // alpha
        }
    }
    return out;
}

// ─── Impl ─────────────────────────────────────────────────────────────────────

struct RdpEncoder::Impl {
    int  last_w = 0;
    int  last_h = 0;
};

// ─── Public API ───────────────────────────────────────────────────────────────

RdpEncoder::RdpEncoder() : impl_(std::make_unique<Impl>()) {}
RdpEncoder::~RdpEncoder() = default;

void RdpEncoder::update_params(const pulsar::core::EncoderParams& p) {
    params_ = p;
}

pulsar::core::AdapterCapabilities RdpEncoder::capabilities() const {
    pulsar::core::AdapterCapabilities c;
    // RDP encoder works on raw BGRA directly; also accepts NV12 (converted internally).
    c.input_formats  = { pulsar::core::PixelFormat::BGRA,
                         pulsar::core::PixelFormat::RGBA,
                         pulsar::core::PixelFormat::NV12 };
    c.output_formats = {};               // opaque RDP PDUs, not a standard codec
    c.supports_dirty_rect = true;        // we encode only dirty_rects
    return c;
}

void RdpEncoder::set_encoded_callback(
        std::function<void(pulsar::core::EncodedPacket)> cb) {
    callback_ = std::move(cb);
}

// ─── submit_frame ─────────────────────────────────────────────────────────────
//
// Builds a Fast-Path Bitmap Update PDU for each dirty rectangle.
// If no dirty rects are provided, a single full-frame rectangle is sent.
// Pixel data is 32 bpp BGRA, written bottom-up (RDP scan order).

void RdpEncoder::submit_frame(pulsar::core::RawFrame frame,
                               pulsar::core::SubmitFlags /*flags*/) {
    if (!callback_ || !frame.buffer) return;

    const int W = frame.width;
    const int H = frame.height;
    if (W <= 0 || H <= 0) return;

    // If input is NV12, convert to BGRA in-place before building the PDU.
    std::vector<uint8_t> bgra_tmp;
    const uint8_t* src = frame.buffer->data();
    if (!src) return;
    if (frame.format == pulsar::core::PixelFormat::NV12) {
        bgra_tmp = nv12_to_bgra(src, W, H);
        src = bgra_tmp.data();
    }

    // Determine rectangles to encode.
    std::vector<pulsar::core::DirtyRect> rects = frame.dirty_rects;
    if (rects.empty()) rects.push_back({0, 0, W, H});

    // Build one Fast-Path Update PDU for all rectangles.
    //
    // Layout:
    //   [1] updateHeader         uint8   (FASTPATH_UPDATE_BITMAP | fragment bits)
    //   [2] size                 uint16  (length of what follows, little-endian)
    //   [3] updateType           uint16  (0x0001 = BITMAP)
    //   [4] numberRectangles     uint16
    //   for each rect:
    //     [5] destLeft/Top/Right/Bottom  uint16 × 4
    //     [6] width/height               uint16 × 2
    //     [7] bitsPerPixel               uint16
    //     [8] flags                      uint16
    //     [9] bitmapLength               uint16
    //    [10] bitmapDataStream           bytes (raw scanlines, bottom-up)

    const int kBpp = 4;  // 32-bit BGRA

    // Pre-compute total payload size to avoid reallocation.
    size_t payload_sz = 2 + 2; // updateType + numberRectangles
    for (const auto& r : rects) {
        const int rw = std::min(r.width,  W - r.x);
        const int rh = std::min(r.height, H - r.y);
        if (rw <= 0 || rh <= 0) continue;
        payload_sz += 4 * 2          // destLeft/Top/Right/Bottom
                    + 2 + 2          // width, height
                    + 2 + 2 + 2      // bitsPerPixel, flags, bitmapLength
                    + static_cast<size_t>(rw * rh * kBpp);
    }

    std::vector<uint8_t> pdu;
    pdu.reserve(3 + payload_sz); // header (1) + size (2) + payload

    // Fast-Path update header
    pdu.push_back(FASTPATH_UPDATE_BITMAP | FASTPATH_FRAGMENT_SINGLE);
    // Size field (2 bytes, LE) — filled in below
    const size_t size_offset = pdu.size();
    pdu.push_back(0); pdu.push_back(0);

    // updateType = BITMAP (1)
    write_le16(pdu, 0x0001);
    // numberRectangles
    write_le16(pdu, static_cast<uint16_t>(rects.size()));

    for (const auto& r : rects) {
        const int rx  = std::max(0, r.x);
        const int ry  = std::max(0, r.y);
        const int rw  = std::min(r.width,  W - rx);
        const int rh  = std::min(r.height, H - ry);
        if (rw <= 0 || rh <= 0) continue;

        const size_t bitmap_len = static_cast<size_t>(rw * rh * kBpp);

        // TS_BITMAP_DATA header fields
        write_le16(pdu, static_cast<uint16_t>(rx));               // destLeft
        write_le16(pdu, static_cast<uint16_t>(ry));               // destTop
        write_le16(pdu, static_cast<uint16_t>(rx + rw - 1));      // destRight
        write_le16(pdu, static_cast<uint16_t>(ry + rh - 1));      // destBottom
        write_le16(pdu, static_cast<uint16_t>(rw));               // width
        write_le16(pdu, static_cast<uint16_t>(rh));               // height
        write_le16(pdu, 32);                                       // bitsPerPixel
        write_le16(pdu, NO_BITMAP_COMPRESSION_HDR);               // flags: no compression
        write_le16(pdu, static_cast<uint16_t>(bitmap_len));       // bitmapLength

        // bitmapDataStream: raw BGRA scanlines, bottom-up order
        // RDP requires bottom-up row order for uncompressed bitmaps.
        const size_t src_stride = static_cast<size_t>(W * kBpp);
        for (int row = rh - 1; row >= 0; --row) {
            const uint8_t* row_ptr = src
                + static_cast<size_t>(ry + row) * src_stride
                + static_cast<size_t>(rx) * kBpp;
            pdu.insert(pdu.end(), row_ptr, row_ptr + static_cast<size_t>(rw * kBpp));
        }
    }

    // Fill in the 2-byte size field (total bytes after the size field itself).
    const uint16_t sz = static_cast<uint16_t>(pdu.size() - size_offset - 2);
    pdu[size_offset]     = static_cast<uint8_t>(sz);
    pdu[size_offset + 1] = static_cast<uint8_t>(sz >> 8);

    // Wrap in an EncodedPacket.
    struct RdpPacketBuffer final : public pulsar::core::PacketBuffer {
        std::vector<uint8_t> d_;
        explicit RdpPacketBuffer(std::vector<uint8_t> d) : d_(std::move(d)) {}
        const uint8_t* data() const override { return d_.data(); }
        size_t         size() const override { return d_.size(); }
    };

    pulsar::core::EncodedPacket pkt;
    pkt.buffer     = std::make_shared<RdpPacketBuffer>(std::move(pdu));
    pkt.is_keyframe = true;  // every frame is independently decodable in RDP
    pkt.pts_us     = frame.pts_us;
    pkt.codec      = pulsar::core::CodecType::H264; // sentinel; transport ignores it
    callback_(std::move(pkt));
}

} // namespace pulsar::encoder::rdp
