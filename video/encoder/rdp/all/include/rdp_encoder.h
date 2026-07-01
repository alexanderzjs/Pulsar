// encoder/rdp/include/encoder/rdp/rdp_encoder.h
// RDP Encoder Adapter — packages raw frames as RDP Bitmap Update PDUs.
// Accepts BGRA or NV12 frames; outputs RDP-framed EncodedPackets consumed
// by RdpTransport::send().
#pragma once

#include "encoder.h"
#include <functional>
#include <memory>

namespace pulsar::encoder::rdp {

class RdpEncoder final : public pulsar::core::IEncoder {
public:
    RdpEncoder();
    ~RdpEncoder() override;

    void submit_frame(pulsar::core::RawFrame frame,
                      pulsar::core::SubmitFlags flags = pulsar::core::SubmitFlags::None) override;
    void set_encoded_callback(std::function<void(pulsar::core::EncodedPacket)> cb) override;
    void update_params(const pulsar::core::EncoderParams& params) override;
    pulsar::core::AdapterCapabilities capabilities() const override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    std::function<void(pulsar::core::EncodedPacket)> callback_;
    pulsar::core::EncoderParams params_{};
};

} // namespace pulsar::encoder::rdp
