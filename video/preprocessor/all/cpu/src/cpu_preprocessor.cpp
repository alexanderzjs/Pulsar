// preprocessor/cpu/src/cpu_preprocessor.cpp
// Software BGRA/RGBA → NV12 colour space conversion (BT.601 full-range).
//
// Y  =  16 + ( 65.481*R + 128.553*G + 24.966*B) / 256  (scaled to 0–235 for limited)
// For full-range (0–255):
//   Y  = (  66*R + 129*G +  25*B + 128) >> 8  + 16
//   Cb = (-38*R -  74*G + 112*B + 128) >> 8  + 128
//   Cr = (112*R -  94*G -  18*B + 128) >> 8  + 128

#include "cpu_preprocessor.h"
#include <cstring>

namespace pulsar::preprocessor::cpu {

pulsar::core::AdapterCapabilities CpuPreprocessor::capabilities() const {
    pulsar::core::AdapterCapabilities caps;
    caps.input_formats  = { pulsar::core::PixelFormat::BGRA,
                             pulsar::core::PixelFormat::RGBA };
    caps.output_formats = { pulsar::core::PixelFormat::NV12 };
    caps.color_spaces   = { pulsar::core::ColorSpace::BT709 };
    return caps;
}

// ── Inline BGRA→NV12 converter ──────────────────────────────────────────────
static void bgra_to_nv12(const uint8_t* bgra, uint8_t* nv12, int w, int h) {
    uint8_t* Y  = nv12;
    uint8_t* UV = nv12 + static_cast<ptrdiff_t>(w * h);

    for (int row = 0; row < h; ++row) {
        for (int col = 0; col < w; ++col) {
            const uint8_t* p = bgra + (row * w + col) * 4;
            const int B = p[0], G = p[1], R = p[2];

            Y[row * w + col] = static_cast<uint8_t>(
                ((66 * R + 129 * G + 25 * B + 128) >> 8) + 16);

            // Chroma: subsample 4:2:0 (one UV sample per 2×2 luma block)
            if ((row & 1) == 0 && (col & 1) == 0) {
                // Average 2×2 block for better chroma quality
                const uint8_t* p01 = (col + 1 < w) ? p + 4       : p;
                const uint8_t* p10 = (row + 1 < h) ? p + w * 4   : p;
                const uint8_t* p11 = (row + 1 < h && col + 1 < w) ? p + w * 4 + 4 : p;

                const int R4 = R + p01[2] + p10[2] + p11[2];
                const int G4 = G + p01[1] + p10[1] + p11[1];
                const int B4 = B + p01[0] + p10[0] + p11[0];

                const int idx = (row / 2) * w + col;
                UV[idx]     = static_cast<uint8_t>(((-38*R4 - 74*G4 + 112*B4 + 4*128) >> 10) + 128);
                UV[idx + 1] = static_cast<uint8_t>(((112*R4 - 94*G4 -  18*B4 + 4*128) >> 10) + 128);
            }
        }
    }
}

// RGBA→NV12: same as BGRA but R and B are swapped in the source.
static void rgba_to_nv12(const uint8_t* rgba, uint8_t* nv12, int w, int h) {
    uint8_t* Y  = nv12;
    uint8_t* UV = nv12 + static_cast<ptrdiff_t>(w * h);

    for (int row = 0; row < h; ++row) {
        for (int col = 0; col < w; ++col) {
            const uint8_t* p = rgba + (row * w + col) * 4;
            const int R = p[0], G = p[1], B = p[2];

            Y[row * w + col] = static_cast<uint8_t>(
                ((66 * R + 129 * G + 25 * B + 128) >> 8) + 16);

            if ((row & 1) == 0 && (col & 1) == 0) {
                const uint8_t* p01 = (col + 1 < w) ? p + 4      : p;
                const uint8_t* p10 = (row + 1 < h) ? p + w * 4  : p;
                const uint8_t* p11 = (row + 1 < h && col + 1 < w) ? p + w*4+4 : p;

                const int R4 = R + p01[0] + p10[0] + p11[0];
                const int G4 = G + p01[1] + p10[1] + p11[1];
                const int B4 = B + p01[2] + p10[2] + p11[2];

                const int idx = (row / 2) * w + col;
                UV[idx]     = static_cast<uint8_t>(((-38*R4 - 74*G4 + 112*B4 + 4*128) >> 10) + 128);
                UV[idx + 1] = static_cast<uint8_t>(((112*R4 - 94*G4 -  18*B4 + 4*128) >> 10) + 128);
            }
        }
    }
}

std::optional<pulsar::core::RawFrame>
CpuPreprocessor::process(pulsar::core::RawFrame frame)
{
    if (!frame.buffer || frame.width <= 0 || frame.height <= 0) return std::nullopt;

    // Already NV12 — pass through without copying.
    if (frame.format == pulsar::core::PixelFormat::NV12) return frame;

    const int w = frame.width, h = frame.height;
    const size_t nv12_size = static_cast<size_t>(w * h * 3 / 2);

    struct NV12Buf final : public pulsar::core::FrameBuffer {
        std::vector<uint8_t> d_;
        explicit NV12Buf(size_t n) : d_(n, 0) {}
        uint8_t* data() const override { return const_cast<uint8_t*>(d_.data()); }
        size_t   size() const override { return d_.size(); }
    };
    auto out = std::make_shared<NV12Buf>(nv12_size);

    if (frame.format == pulsar::core::PixelFormat::BGRA) {
        bgra_to_nv12(frame.buffer->data(), out->d_.data(), w, h);
    } else if (frame.format == pulsar::core::PixelFormat::RGBA) {
        rgba_to_nv12(frame.buffer->data(), out->d_.data(), w, h);
    } else {
        // Unknown format: zero-fill NV12 (grey frame) rather than crash.
        // This branch should not be reached given capabilities() declaration.
        std::memset(out->d_.data(), 128, nv12_size);
    }

    frame.buffer = std::move(out);
    frame.format = pulsar::core::PixelFormat::NV12;
    frame.color_info.color_space = pulsar::core::ColorSpace::BT709;
    return frame;
}

} // namespace pulsar::preprocessor::cpu
