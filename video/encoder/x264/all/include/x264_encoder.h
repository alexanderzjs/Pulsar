#pragma once

#include "encoder.h"
#include <functional>
#include <memory>

namespace pulsar::encoder::x264 {

// Software H.264 encoder.
// Implementation uses libavcodec (libx264) as its backend.
// Used as the final fallback when no hardware encoder is available.
class X264Encoder final : public pulsar::core::IEncoder {
public:
    X264Encoder();
    ~X264Encoder() override;

    void submit_frame(pulsar::core::RawFrame frame,
                      pulsar::core::SubmitFlags flags = pulsar::core::SubmitFlags::None) override;
    void set_encoded_callback(std::function<void(pulsar::core::EncodedPacket)> cb) override;
    void update_params(const pulsar::core::EncoderParams& params) override;
    pulsar::core::AdapterCapabilities capabilities() const override;

private:
    bool open_context(int width, int height);
    void close();

    struct Impl;
    std::unique_ptr<Impl> impl_;

    std::function<void(pulsar::core::EncodedPacket)> callback_;
    pulsar::core::EncoderParams params_{};
    bool open_ = false;
};

} // namespace pulsar::encoder::x264
