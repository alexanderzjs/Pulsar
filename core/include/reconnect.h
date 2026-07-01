#pragma once

namespace pulsar::core {

enum class PipelineState {
    Starting,     // being initialised
    Running,      // normal streaming
    Suspended,    // transport disconnected; capture paused; waiting for reconnect
    Recovering,   // transport reconnected; sending forced keyframe
    Stopped       // session destroyed
};

// Controls how the pipeline behaves after a transport disconnection.
struct ReconnectPolicy {
    int  suspend_timeout_ms        = 30000; // destroy session after this; -1 = wait forever
    int  reconnect_interval_ms     = 1000;  // how often to poll for a new connection
    int  max_retries               = -1;    // -1 = unlimited
    bool force_keyframe_on_resume  = true;  // send IDR immediately when transport comes back
};

} // namespace pulsar::core
