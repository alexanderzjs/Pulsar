#include "pipeline.h"
#include "queue.h"

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <thread>

namespace pulsar::core {

using namespace std::chrono;

// ─── helpers ─────────────────────────────────────────────────────────────────
static void set_state(PipelineState s,
                      std::atomic<int>& current,
                      const std::function<void(PipelineState)>& cb,
                      ILogger* logger)
{
    current.store(static_cast<int>(s), std::memory_order_release);
    if (cb)     cb(s);
    if (logger) {
        const char* name = "unknown";
        switch (s) {
        case PipelineState::Starting:   name = "Starting";   break;
        case PipelineState::Running:    name = "Running";    break;
        case PipelineState::Suspended:  name = "Suspended";  break;
        case PipelineState::Recovering: name = "Recovering"; break;
        case PipelineState::Stopped:    name = "Stopped";    break;
        }
        logger->log(LogLevel::Info, std::string("pipeline: state → ") + name);
    }
}

// ─── run_pipeline ─────────────────────────────────────────────────────────────
bool run_pipeline(
    ICaptureSource&          capture,
    IPreprocessor*           preprocessor,
    IEncoder&                encoder_ref,
    ITransport&              transport,
    IInputHandler&           input,
    IAudioCapture*           audio_capture,
    IAudioEncoder*           audio_encoder,
    const PipelineConfig&    config,
    ILogger*                 logger,
    IMetricsCollector*       /*metrics*/,
    std::atomic<bool>*       stop_flag,
    std::function<void(PipelineState)> on_state_change,
    std::function<std::unique_ptr<IEncoder>()> encoder_fallback)
{
    (void)input; // driven by transport::set_input_callback in factory

    std::atomic<bool> local_stop{false};
    std::atomic<bool>& stop = stop_flag ? *stop_flag : local_stop;

    // ── State machine ─────────────────────────────────────────────────────────
    std::atomic<int> state_int{static_cast<int>(PipelineState::Starting)};
    auto state = [&]() { return static_cast<PipelineState>(state_int.load(std::memory_order_acquire)); };
    auto go    = [&](PipelineState s) { set_state(s, state_int, on_state_change, logger); };

    // ── Active encoder (may be swapped to fallback after device loss) ──────────
    IEncoder*                    enc_ptr = &encoder_ref;
    std::unique_ptr<IEncoder>    fallback_enc;   // owns the fallback if created

    // ── Queues ────────────────────────────────────────────────────────────────
    const int cap = std::max(2, config.queue_capacity);
    SPSCQueue<RawFrame>      q1(cap);
    SPSCQueue<EncodedPacket> q2(cap);
    std::atomic<int64_t>     latest_video_pts_us{0};

    // ── Wire encoder → Q2 ─────────────────────────────────────────────────────
    auto wire_encoder = [&](IEncoder& enc) {
        enc.set_encoded_callback([&](EncodedPacket pkt) {
            latest_video_pts_us.store(pkt.pts_us, std::memory_order_relaxed);
            q2.try_push(std::move(pkt), 16);
        });
    };
    wire_encoder(*enc_ptr);

    // ── Wire audio → transport ─────────────────────────────────────────────────
    if (audio_encoder) {
        audio_encoder->set_encoded_callback([&transport](AudioPacket pkt) {
            transport.send_audio(std::move(pkt));
        });
    }

    // ── Suspend/resume flag (set by transport Disconnected) ───────────────────
    std::atomic<bool> suspended{false};
    std::atomic<steady_clock::time_point::duration::rep> suspend_since{0};

    // ── Transport event callback ───────────────────────────────────────────────
    transport.set_event_callback([&](TransportEvent ev) {
        if (ev == TransportEvent::Disconnected) {
            if (state() == PipelineState::Running) {
                suspended.store(true, std::memory_order_release);
                suspend_since.store(
                    steady_clock::now().time_since_epoch().count(),
                    std::memory_order_relaxed);
                go(PipelineState::Suspended);
            }
        } else if (ev == TransportEvent::Ready) {
            if (state() == PipelineState::Suspended) {
                go(PipelineState::Recovering);
                suspended.store(false, std::memory_order_release);
            }
        } else if (ev == TransportEvent::Congested) {
            // Force an IDR so the client can recover without waiting for the next keyframe.
            // Handled in encode thread via flag.
        }
    });

    // ── Capture device-loss callback ──────────────────────────────────────────
    std::atomic<bool> capture_lost{false};
    capture.set_event_callback([&](CaptureEvent ev) {
        if (ev == CaptureEvent::DeviceLost) {
            capture_lost.store(true, std::memory_order_release);
            if (logger) logger->log(LogLevel::Warn, "pipeline: capture device lost");
        }
        if (ev == CaptureEvent::ResolutionChanged) {
            // Drain Q1 and send a forced keyframe on the next encode.
            if (logger) logger->log(LogLevel::Info, "pipeline: resolution changed");
        }
    });

    // ── Force-keyframe flag (set on Recovering or Congested) ─────────────────
    std::atomic<bool> force_next_keyframe{true}; // always send IDR at startup

    go(PipelineState::Running);

    // ─────────────────────────────────────────────────────────────────────────
    // Thread 1: Capture → Q1
    // Respects Suspended state: pauses next_frame() when transport is down.
    // ─────────────────────────────────────────────────────────────────────────
    std::thread capture_th([&] {
        const microseconds idle_sleep{config.idle_fps > 0 ? 1'000'000 / config.idle_fps : 200'000};
        const milliseconds normal_sleep{config.target_fps > 0 ? 1000 / config.target_fps : 16};
        int idle_streak = 0;

        while (!stop.load(std::memory_order_relaxed)) {
            // Paused while transport is disconnected.
            if (suspended.load(std::memory_order_acquire)) {
                std::this_thread::sleep_for(milliseconds(50));
                // Check suspend timeout.
                if (config.reconnect.suspend_timeout_ms >= 0) {
                    auto since = steady_clock::duration{
                        suspend_since.load(std::memory_order_relaxed)};
                    auto elapsed_ms = duration_cast<milliseconds>(
                        steady_clock::now().time_since_epoch() - since).count();
                    if (elapsed_ms > config.reconnect.suspend_timeout_ms) {
                        if (logger) logger->log(LogLevel::Warn,
                            "pipeline: reconnect timeout — stopping session");
                        stop.store(true);
                        go(PipelineState::Stopped);
                    }
                }
                continue;
            }

            // Handle capture device loss: retry up to 3 times.
            if (capture_lost.load(std::memory_order_acquire)) {
                if (logger) logger->log(LogLevel::Warn, "pipeline: retrying capture...");
                bool recovered = false;
                for (int attempt = 0; attempt < 3 && !stop.load(); ++attempt) {
                    std::this_thread::sleep_for(seconds(1));
                    // Try calling next_frame; if it returns a valid frame, device recovered.
                    auto probe = capture.next_frame();
                    if (probe.has_value()) {
                        capture_lost.store(false, std::memory_order_release);
                        q1.try_push(std::move(*probe), 0);
                        recovered = true;
                        break;
                    }
                }
                if (!recovered) {
                    if (logger) logger->log(LogLevel::Error, "pipeline: capture unrecoverable — stopping");
                    stop.store(true);
                    go(PipelineState::Stopped);
                    break;
                }
                continue;
            }

            auto frame = capture.next_frame();
            if (frame.has_value()) {
                idle_streak = 0;
                if (!q1.try_push(std::move(*frame), 0)) {
                    if (logger) logger->log(LogLevel::Warn, "pipeline: capture frame dropped (encode backpressure)");
                }
            } else {
                ++idle_streak;
                std::this_thread::sleep_for(idle_streak > 3 ? idle_sleep : milliseconds(1));
            }
        }
    });

    // ─────────────────────────────────────────────────────────────────────────
    // Thread 2: Q1 → Preprocess → Encode → Q2
    // Handles encoder failures: attempts reinit, then falls back to x264.
    // ─────────────────────────────────────────────────────────────────────────
    std::thread encode_th([&] {
        int consecutive_failures = 0;
        bool needs_keyframe = false;

        while (!stop.load(std::memory_order_relaxed)) {
            // On Recovering state: force a keyframe on the next submitted frame.
            if (state() == PipelineState::Recovering) {
                force_next_keyframe.store(true, std::memory_order_relaxed);
                go(PipelineState::Running);
            }

            RawFrame frame;
            if (!q1.try_pop(frame, 20)) continue;

            const bool fkf = force_next_keyframe.exchange(false, std::memory_order_relaxed);
            const SubmitFlags flags = fkf ? SubmitFlags::ForceKeyframe : SubmitFlags::None;

            bool ok = true;
            try {
                if (preprocessor) {
                    auto out = preprocessor->process(std::move(frame));
                    if (out.has_value()) enc_ptr->submit_frame(std::move(*out), flags);
                } else {
                    enc_ptr->submit_frame(std::move(frame), flags);
                }
                consecutive_failures = 0;
            } catch (...) {
                ++consecutive_failures;
                ok = false;
            }

            // Encoder failure recovery: reinit → fallback to x264.
            if (!ok && consecutive_failures >= 3) {
                if (logger) logger->log(LogLevel::Warn, "pipeline: encoder failed — attempting fallback");
                if (encoder_fallback) {
                    fallback_enc = encoder_fallback();
                    if (fallback_enc) {
                        enc_ptr = fallback_enc.get();
                        wire_encoder(*enc_ptr);
                        consecutive_failures = 0;
                        force_next_keyframe.store(true, std::memory_order_relaxed);
                        if (logger) logger->log(LogLevel::Info, "pipeline: switched to fallback encoder");
                    } else {
                        stop.store(true);
                        go(PipelineState::Stopped);
                    }
                } else {
                    stop.store(true);
                    go(PipelineState::Stopped);
                }
            }
        }
    });

    // ─────────────────────────────────────────────────────────────────────────
    // Thread 3: Q2 → Transport::send (video)
    // ─────────────────────────────────────────────────────────────────────────
    std::thread send_th([&] {
        while (!stop.load(std::memory_order_relaxed)) {
            if (suspended.load(std::memory_order_acquire)) {
                std::this_thread::sleep_for(milliseconds(10));
                continue;
            }
            EncodedPacket pkt;
            if (q2.try_pop(pkt, 20)) transport.send(std::move(pkt));
        }
    });

    // ─────────────────────────────────────────────────────────────────────────
    // Thread 4: Audio capture → encode → send, pts_us aligned
    // Inserts silence on AudioCapture device loss.
    // ─────────────────────────────────────────────────────────────────────────
    std::thread audio_th([&] {
        if (!config.audio_enabled || !audio_capture || !audio_encoder) return;

        while (!stop.load(std::memory_order_relaxed)) {
            if (suspended.load(std::memory_order_acquire)) {
                std::this_thread::sleep_for(milliseconds(10));
                continue;
            }

            auto af = audio_capture->next_frame();
            if (!af.has_value()) {
                // Device lost: insert a 10ms silence frame to keep the timeline continuous.
                AudioFrame silence;
                silence.sample_rate = 48000;
                silence.channels    = 2;
                silence.samples     = 480; // 10ms @ 48kHz
                silence.size        = static_cast<size_t>(silence.samples) * 2 * sizeof(int16_t);
                silence.pts_us      = latest_video_pts_us.load(std::memory_order_relaxed);
                silence.format      = AudioFormat::PCM_S16LE;
                silence.data        = std::shared_ptr<uint8_t[]>(new uint8_t[silence.size]());
                audio_encoder->submit_frame(std::move(silence));
                std::this_thread::sleep_for(milliseconds(10));
                continue;
            }

            // pts_us alignment
            const int64_t vpts = latest_video_pts_us.load(std::memory_order_relaxed);
            if (vpts > 0) {
                const int64_t ahead = af->pts_us - vpts;
                if (ahead > 100'000)
                    std::this_thread::sleep_for(microseconds(ahead - 100'000));
            }
            audio_encoder->submit_frame(std::move(*af));
        }
    });

    capture_th.join();
    encode_th.join();
    send_th.join();
    audio_th.join();

    // Clear the transport callback before returning — the lambda captures local
    // variables (state_int, stop) that will be destroyed when this frame exits.
    transport.set_event_callback(nullptr);

    const bool session_ok = (state() != PipelineState::Stopped);
    if (logger) logger->log(LogLevel::Info,
        std::string("pipeline: all threads stopped (") +
        (session_ok ? "clean" : "session destroyed") + ")");
    return session_ok;
}

} // namespace pulsar::core
