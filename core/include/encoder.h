#pragma once

#include "capabilities.h"
#include "frame.h"

#include <cstdint>
#include <functional>

namespace pulsar::core {

struct GopConfig {
    int  max_b_frames       = 0;
    int  gop_size           = 120;
    bool low_latency_preset = true;
    bool intra_refresh      = false;
    int  target_fps         = 60;
};

enum class QualityPreset { LowLatency, Balanced, HighQuality };

struct EncoderParams {
    int           bitrate_kbps = 8000;
    int           fps          = 60;
    GopConfig     gop{};
    QualityPreset preset = QualityPreset::LowLatency;
};

enum class SubmitFlags : uint32_t {
    None          = 0,
    ForceKeyframe = 1u << 0,
};

class IEncoder : public ICapabilityProvider {
public:
    virtual ~IEncoder() = default;
    virtual void submit_frame(RawFrame frame, SubmitFlags flags = SubmitFlags::None) = 0;
    virtual void set_encoded_callback(std::function<void(EncodedPacket)> cb) = 0;
    virtual void update_params(const EncoderParams& params) = 0;
};

} // namespace pulsar::core
