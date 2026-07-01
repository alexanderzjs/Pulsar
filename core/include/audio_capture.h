#pragma once

#include "audio.h"
#include "capabilities.h"

#include <functional>
#include <optional>

namespace pulsar::core {

enum class AudioCaptureEvent { DeviceLost, FormatChanged };

class IAudioCapture : public ICapabilityProvider {
public:
    virtual ~IAudioCapture() = default;
    virtual std::optional<AudioFrame> next_frame() = 0;
    virtual void set_event_callback(std::function<void(AudioCaptureEvent)> cb) = 0;
};

} // namespace pulsar::core
