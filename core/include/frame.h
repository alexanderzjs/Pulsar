#pragma once

#include <cstdint>
#include <memory>
#include <vector>

namespace pulsar::core {

enum class PixelFormat  { BGRA, RGBA, NV12, YUV420 };
enum class CodecType    { H264, H265, AV1 };
enum class ColorSpace   { BT709, BT2020 };
enum class TransferFunc { SDR, PQ, HLG };

struct ColorInfo {
    ColorSpace   color_space   = ColorSpace::BT709;
    TransferFunc transfer_func = TransferFunc::SDR;
    float        max_luminance = 100.0f;
    float        min_luminance = 0.0f;
};

struct DirtyRect {
    int x, y, width, height;
};

struct FrameBuffer {
    virtual ~FrameBuffer() = default;
    virtual uint8_t* data()          const = 0;
    virtual size_t   size()          const = 0;
    virtual void*    native_handle() const { return nullptr; }
};

struct PacketBuffer {
    virtual ~PacketBuffer() = default;
    virtual const uint8_t* data()          const = 0;
    virtual size_t         size()          const = 0;
    virtual void*          native_handle() const { return nullptr; }
};

struct RawFrame {
    std::shared_ptr<FrameBuffer> buffer;
    int         width  = 0;
    int         height = 0;
    int64_t     pts_us = 0;
    PixelFormat format = PixelFormat::BGRA;
    ColorInfo   color_info{};
    std::vector<DirtyRect> dirty_rects;
};

struct EncodedPacket {
    std::shared_ptr<PacketBuffer> buffer;
    bool      is_keyframe = false;
    int64_t   pts_us      = 0;
    CodecType codec       = CodecType::H264;
};

} // namespace pulsar::core
