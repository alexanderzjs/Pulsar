#pragma once

#include "capabilities.h"
#include "frame.h"

#include <optional>

namespace pulsar::core {

class IPreprocessor : public ICapabilityProvider {
public:
    virtual ~IPreprocessor() = default;
    // Returns nullopt to pass the frame through unchanged.
    virtual std::optional<RawFrame> process(RawFrame frame) = 0;
};

} // namespace pulsar::core
