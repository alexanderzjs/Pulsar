#include "preprocessor.h"

namespace pulsar::preprocessor::dmabuf {

// DMA-BUF zero-copy importer: passes NV12/BGRA frames whose FrameBuffer
// carries a native dmabuf handle directly to the encoder without a CPU copy.
class DmabufImporter final : public pulsar::core::IPreprocessor {
public:
    std::optional<pulsar::core::RawFrame> process(pulsar::core::RawFrame frame) override;
    pulsar::core::AdapterCapabilities capabilities() const override;
};

} // namespace pulsar::preprocessor::dmabuf
