#pragma once

#include "capture.h"

#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>

// PipeWire stream.h is needed for pw_stream_state in the callback signatures.
#include <pipewire/stream.h>

struct pw_main_loop;
struct pw_context;
struct pw_core;

namespace pulsar::capture::pipewire {

// PipeWire screen capture source.
// Connects to a PipeWire portal stream and delivers NV12 frames.
class PipeWireCapture final : public pulsar::core::ICaptureSource {
public:
    explicit PipeWireCapture();
    ~PipeWireCapture() override;

    std::optional<pulsar::core::RawFrame>    next_frame()   override;
    std::optional<pulsar::core::CursorState> next_cursor()  override;
    std::vector<pulsar::core::DisplayInfo>   enumerate_displays() const override;
    void select_display(int index) override;
    int  display_refresh_rate() const override;
    void set_event_callback(std::function<void(pulsar::core::CaptureEvent)> cb) override;
    pulsar::core::AdapterCapabilities capabilities() const override;

    // PipeWire stream callbacks (called from the PW event loop thread).
    static void on_process(void* userdata);
    static void on_state_changed(void* userdata,
                                 enum pw_stream_state old_state,
                                 enum pw_stream_state state,
                                 const char* error);

private:
    void event_loop_thread();
    void teardown();

    static constexpr int kDefaultWidth  = 1280;
    static constexpr int kDefaultHeight = 720;
    static constexpr int kDefaultFps    = 60;

    pw_main_loop* loop_    = nullptr;
    pw_context*   context_ = nullptr;
    pw_core*      core_    = nullptr;
    pw_stream*    stream_  = nullptr;

    std::thread               loop_thread_;
    mutable std::mutex        frame_mutex_;
    std::condition_variable   frame_cv_;
    std::optional<pulsar::core::RawFrame> pending_frame_;
    std::atomic<bool>         connected_{false};
    std::atomic<bool>         stop_{false};

    int  display_index_  = 0;
    std::function<void(pulsar::core::CaptureEvent)> event_cb_;
};

} // namespace pulsar::capture::pipewire
