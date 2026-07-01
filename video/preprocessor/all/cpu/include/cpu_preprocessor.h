#include "preprocessor.h"

namespace pulsar::preprocessor::cpu {

// CPU-based format converter: BGRA/RGBA → NV12 (libyuv stub for MVP).
class CpuPreprocessor final : public pulsar::core::IPreprocessor {
public:
    std::optional<pulsar::core::RawFrame> process(pulsar::core::RawFrame frame) override;
    pulsar::core::AdapterCapabilities capabilities() const override;
};

} // namespace pulsar::preprocessor::cpu
