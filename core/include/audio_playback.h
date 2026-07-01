#pragma once

#include "audio.h"
#include "capabilities.h"
#include <functional>

namespace pulsar::core {

// Plays back audio received from the remote client's microphone.
class IAudioPlayback : public ICapabilityProvider {
public:
    virtual ~IAudioPlayback() = default;
    // Push a decoded audio frame (from client mic) to the local playback device.
    virtual void play(AudioFrame frame) = 0;
    virtual void set_volume(float volume) = 0;  // 0.0 – 1.0
    virtual void mute(bool muted) = 0;
};

} // namespace pulsar::core
