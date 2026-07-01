#include "dmabuf_importer.h"

namespace pulsar::preprocessor::dmabuf {

pulsar::core::AdapterCapabilities DmabufImporter::capabilities() const {
    pulsar::core::AdapterCapabilities caps;
    caps.input_formats   = { pulsar::core::PixelFormat::BGRA,
                              pulsar::core::PixelFormat::RGBA,
                              pulsar::core::PixelFormat::NV12 };
    caps.output_formats  = { pulsar::core::PixelFormat::NV12 };
    caps.color_spaces    = { pulsar::core::ColorSpace::BT709,
                              pulsar::core::ColorSpace::BT2020 };
    caps.supports_dmabuf = true;
    return caps;
}

std::optional<pulsar::core::RawFrame>
DmabufImporter::process(pulsar::core::RawFrame frame)
{
    if (!frame.buffer || frame.width <= 0 || frame.height <= 0)
        return std::nullopt;

    // Normalise format to NV12.  If the buffer carries a native dmabuf fd
    // (native_handle() != nullptr) the encoder can import it via
    // vaCreateSurfaces(DRM_PRIME) and skip the CPU upload entirely.
    // Phase 2 TODO: when native_handle() is valid, wrap it as a VASurface
    // so the VAAPI encoder calls vaBeginPicture directly on the surface —
    // completing the full zero-copy PipeWire → VAAPI path.
    frame.format = pulsar::core::PixelFormat::NV12;
    frame.color_info.color_space = pulsar::core::ColorSpace::BT709;
    return frame;
}

} // namespace pulsar::preprocessor::dmabuf
