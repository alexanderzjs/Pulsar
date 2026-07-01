#pragma once

#include "audio.h"
#include "capabilities.h"

#include <functional>

namespace pulsar::core {

class IAudioEncoder : public ICapabilityProvider {
public:
    virtual ~IAudioEncoder() = default;
    virtual void submit_frame(AudioFrame frame) = 0;
    virtual void set_encoded_callback(std::function<void(AudioPacket)> cb) = 0;
};

} // namespace pulsar::core
