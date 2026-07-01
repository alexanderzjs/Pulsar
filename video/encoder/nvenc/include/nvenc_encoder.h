#pragma once

#include "encoder.h"
#include <functional>
#include <memory>
#include <string>

namespace pulsar::encoder::nvenc {

// Returns true if h264_nvenc or hevc_nvenc can be opened via libavcodec.
bool nvenc_is_available();

// NVIDIA NVENC hardware encoder.
// Implementation uses libavcodec (h264_nvenc / hevc_nvenc) as its backend,
// which handles CUDA context management and NVENC session lifecycle.
// Falls back gracefully: if NVENC is unavailable, submit_frame() is a no-op.
class NvencEncoder final : public pulsar::core::IEncoder {
public:
    // codec: "h264" (default) or "hevc"
    explicit NvencEncoder(const std::string& codec = "h264");
    ~NvencEncoder() override;

    void submit_frame(pulsar::core::RawFrame frame,
                      pulsar::core::SubmitFlags flags = pulsar::core::SubmitFlags::None) override;
    void set_encoded_callback(std::function<void(pulsar::core::EncodedPacket)> cb) override;
    void update_params(const pulsar::core::EncoderParams& params) override;
    pulsar::core::AdapterCapabilities capabilities() const override;

    bool is_initialised() const { return initialised_; }

private:
    bool init(const std::string& codec);
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

} // namespace pulsar::encoder::nvenc
