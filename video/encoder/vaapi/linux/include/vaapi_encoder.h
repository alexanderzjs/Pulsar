#pragma once

#include "encoder.h"
#include <functional>
#include <memory>
#include <string>

namespace pulsar::encoder::vaapi {

// Returns true if h264_vaapi or hevc_vaapi can be opened via libavcodec.
bool vaapi_is_available();

// VAAPI hardware encoder (Intel / AMD / NVIDIA with appropriate driver).
// Implementation uses libavcodec (h264_vaapi / hevc_vaapi).
class VaapiEncoder final : public pulsar::core::IEncoder {
public:
    explicit VaapiEncoder(const std::string& codec = "h264");
    ~VaapiEncoder() override;

    void submit_frame(pulsar::core::RawFrame frame,
                      pulsar::core::SubmitFlags flags = pulsar::core::SubmitFlags::None) override;
    void set_encoded_callback(std::function<void(pulsar::core::EncodedPacket)> cb) override;
    void update_params(const pulsar::core::EncoderParams& params) override;
    pulsar::core::AdapterCapabilities capabilities() const override;

    // Legacy probe for factory compatibility.
    static bool is_available() { return vaapi_is_available(); }
    bool is_initialised() const { return initialised_; }

private:
    void teardown();
    bool open_context(int width, int height);
    bool encode_nv12(const uint8_t* data, int width, int height,
                     int64_t pts_us, bool force_idr);

    struct Impl;
    std::unique_ptr<Impl> impl_;

    std::function<void(pulsar::core::EncodedPacket)> callback_;
    pulsar::core::EncoderParams params_{};
    bool initialised_ = false;
    bool use_hevc_    = false;
    bool open_        = false;
};

} // namespace pulsar::encoder::vaapi
