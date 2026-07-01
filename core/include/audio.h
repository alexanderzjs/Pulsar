#pragma once

#include <cstdint>
#include <memory>

namespace pulsar::core {

enum class AudioFormat { PCM_S16LE, PCM_F32LE };
enum class AudioCodec  { OPUS, AAC };

struct AudioFrame {
    std::shared_ptr<uint8_t[]> data;
    size_t      size       = 0;
    int         sample_rate = 48000;
    int         channels   = 2;
    int         samples    = 0;
    int64_t     pts_us     = 0;
    AudioFormat format     = AudioFormat::PCM_S16LE;
};

struct AudioPacket {
    std::shared_ptr<uint8_t[]> data;
    size_t    size   = 0;
    int64_t   pts_us = 0;
    AudioCodec codec = AudioCodec::OPUS;
};

} // namespace pulsar::core
