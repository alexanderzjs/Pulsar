#pragma once

#include "preprocessor.h"
#include <memory>
#include <optional>

namespace pulsar::preprocessor::gpu {

// GPU format converter: BGRA/RGBA → NV12 via VA-API VPP.
// Falls back to CPU path when VA-API VPP is unavailable.
class GpuPreprocessor final : public pulsar::core::IPreprocessor {
public:
    GpuPreprocessor();
    ~GpuPreprocessor() override;

    std::optional<pulsar::core::RawFrame> process(pulsar::core::RawFrame frame) override;
    pulsar::core::AdapterCapabilities capabilities() const override;

private:
    bool open_vpp(int width, int height);
    void close_vpp();

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace pulsar::preprocessor::gpu
