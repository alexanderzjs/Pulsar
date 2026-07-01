#pragma once

#include "frame.h"
#include <vector>

namespace pulsar::core {

struct AdapterCapabilities {
    std::vector<PixelFormat> input_formats;
    std::vector<PixelFormat> output_formats;
    std::vector<ColorSpace>  color_spaces;
    bool supports_dmabuf             = false;
    bool supports_gpu_preprocessing  = false;
    bool supports_async_encode       = false;
    bool supports_dirty_rect         = false;
    bool supports_hdr                = false;
    bool supports_headless           = false;
    bool requires_display            = false;
};

class ICapabilityProvider {
public:
    virtual ~ICapabilityProvider() = default;
    virtual AdapterCapabilities capabilities() const = 0;
};

} // namespace pulsar::core
