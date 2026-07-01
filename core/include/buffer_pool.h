#pragma once

#include "frame.h"
#include <memory>

namespace pulsar::core {

class IBufferPool {
public:
    virtual ~IBufferPool() = default;
    virtual std::shared_ptr<FrameBuffer> acquire() = 0;
};

} // namespace pulsar::core
