#pragma once

#include "audio_capture.h"
#include "audio_encoder.h"
#include "capture.h"
#include "encoder.h"
#include "input.h"
#include "logger.h"
#include "metrics.h"
#include "preprocessor.h"
#include "reconnect.h"
#include "thread_config.h"
#include "transport.h"

#include <atomic>
#include <functional>
#include <memory>

namespace pulsar::core {

struct PipelineConfig {
    // Video
    int  queue_capacity  = 4;
    int  idle_fps        = 5;
    int  target_fps      = 60;

    // Audio/video sync
    bool audio_enabled   = true;

    // Reconnect behaviour
    ReconnectPolicy reconnect{};

    // Threading
    ThreadConfig capture_thread  { .priority = 80, .cpu_affinity = 0  };
    ThreadConfig encode_thread   { .priority = 70, .cpu_affinity = 2  };
    ThreadConfig transport_thread{ .priority = 50, .cpu_affinity = -1 };
    ThreadConfig audio_thread    { .priority = 60, .cpu_affinity = -1 };
};

// Blocking call: runs the encode/transport loop until stopped or session destroyed.
// Returns true on clean stop, false if the session should be permanently destroyed
// (reconnect timeout exceeded).
bool run_pipeline(
    ICaptureSource&          capture,
    IPreprocessor*           preprocessor,   // nullable — direct pass-through
    IEncoder&                encoder,
    ITransport&              transport,
    IInputHandler&           input,
    IAudioCapture*           audio_capture,  // nullable
    IAudioEncoder*           audio_encoder,  // nullable
    const PipelineConfig&    config,
    ILogger*                 logger         = nullptr,
    IMetricsCollector*       metrics        = nullptr,
    std::atomic<bool>*       stop_flag      = nullptr,
    // Optional callbacks for state machine transitions
    std::function<void(PipelineState)> on_state_change = nullptr,
    // Encoder fallback factory: called when the primary encoder fails.
    // Return a replacement encoder or nullptr to stop the session.
    std::function<std::unique_ptr<IEncoder>()> encoder_fallback = nullptr
);

} // namespace pulsar::core
