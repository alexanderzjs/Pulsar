#pragma once

#include "audio_encoder.h"
#include <functional>
#include <memory>

namespace pulsar::audio::encoder::opus {

// Opus audio encoder using vendored libopus [static].
class OpusEncoder final : public pulsar::core::IAudioEncoder {
public:
    OpusEncoder();
    ~OpusEncoder() override;

    void submit_frame(pulsar::core::AudioFrame frame) override;
    void set_encoded_callback(std::function<void(pulsar::core::AudioPacket)> cb) override;
    pulsar::core::AdapterCapabilities capabilities() const override;

    // Dynamically update encoder bitrate (e.g. on transport congestion).
    void set_bitrate_kbps(int kbps);

private:
    bool open(int sample_rate, int channels);
    void close();

    struct Impl;
    std::unique_ptr<Impl> impl_;
    std::function<void(pulsar::core::AudioPacket)> callback_;
};

} // namespace pulsar::audio::encoder::opus
