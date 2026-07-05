#pragma once
#include "audio.h"
#include <optional>
#include <string>

namespace pulsar::core {

struct MixerClientConfig {
    std::string client_id;
    float       volume = 1.0f; // 0.0~2.0
    bool        muted  = false;
};

// Server-side audio mixer for multi-client voice chat (Party Chat).
// Each client's mic frames are pushed in; mix_for() returns the mixed
// result excluding the requesting client (to avoid echo).
class IAudioMixer {
public:
    virtual ~IAudioMixer() = default;
    virtual void add_client(const MixerClientConfig& cfg) = 0;
    virtual void remove_client(const std::string& client_id) = 0;
    virtual void push(const std::string& client_id, const AudioFrame& frame) = 0;
    virtual std::optional<AudioFrame> mix_for(const std::string& client_id) = 0;
    virtual void set_volume(const std::string& client_id, float volume) = 0;
    virtual void set_muted(const std::string& client_id, bool muted) = 0;
};

} // namespace pulsar::core
