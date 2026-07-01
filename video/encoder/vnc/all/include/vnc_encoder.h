// encoder/vnc/include/encoder/vnc/vnc_encoder.h
// VNC Encoder Adapter — packages raw BGRA frames as RFB FramebufferUpdate PDUs.
// Directly accepts BGRA; no NV12 conversion needed (VNC works on raw pixels).
#pragma once

#include "encoder.h"
#include <functional>
#include <memory>

namespace pulsar::encoder::vnc {

class VncEncoder final : public pulsar::core::IEncoder {
public:
    VncEncoder();
    ~VncEncoder() override;

    void submit_frame(pulsar::core::RawFrame frame,
                      pulsar::core::SubmitFlags flags = pulsar::core::SubmitFlags::None) override;
    void set_encoded_callback(std::function<void(pulsar::core::EncodedPacket)> cb) override;
    void update_params(const pulsar::core::EncoderParams& params) override;
    pulsar::core::AdapterCapabilities capabilities() const override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    std::function<void(pulsar::core::EncodedPacket)> callback_;
};

} // namespace pulsar::encoder::vnc
