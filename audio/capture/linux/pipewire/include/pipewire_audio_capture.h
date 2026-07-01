#pragma once

#include "audio_capture.h"

#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <queue>
#include <thread>

#include <pipewire/stream.h>

struct pw_main_loop;
struct pw_context;
struct pw_core;
struct pw_stream;

namespace pulsar::audio::capture::pipewire {

class PipeWireAudioCapture final : public pulsar::core::IAudioCapture {
public:
    explicit PipeWireAudioCapture(int frame_budget = -1);
    ~PipeWireAudioCapture() override;

    std::optional<pulsar::core::AudioFrame> next_frame() override;
    void set_event_callback(std::function<void(pulsar::core::AudioCaptureEvent)> cb) override;
    pulsar::core::AdapterCapabilities capabilities() const override;

    // PipeWire callbacks (public so static free functions can call them)
    static void on_process(void* data);
    static void on_state_changed(void* data, enum pw_stream_state old_state,
                                  enum pw_stream_state state, const char* error);

    static constexpr size_t kMaxQueueDepth = 32;

private:
    void teardown();

    pw_main_loop* loop_   = nullptr;
    pw_context*   ctx_    = nullptr;
    pw_core*      core_   = nullptr;
    pw_stream*    stream_ = nullptr;

    std::thread              loop_thread_;
    mutable std::mutex       q_mtx_;
    std::condition_variable  q_cv_;
    std::queue<pulsar::core::AudioFrame> queue_;
    bool connected_ = false;
    bool stop_req_  = false;

    std::function<void(pulsar::core::AudioCaptureEvent)> event_cb_;
};

} // namespace pulsar::audio::capture::pipewire
