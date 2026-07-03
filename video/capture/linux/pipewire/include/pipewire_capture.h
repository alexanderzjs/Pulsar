#pragma once

#include "capture.h"

#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

// Forward-declare sd_bus so we don't pull in systemd headers in the public header.
struct sd_bus;

// PipeWire stream.h is needed for pw_stream_state in the callback signatures.
#include <pipewire/stream.h>

struct pw_main_loop;
struct pw_context;
struct pw_core;

namespace pulsar::capture::pipewire {

// Shared portal authorization result.
// Created once at server startup; passed to all capture instances so the
// xdg-desktop-portal permission dialog only appears once.
struct PortalSession {
    uint32_t node_id = static_cast<uint32_t>(-1);  // PW_ID_ANY
    sd_bus*  bus     = nullptr;  // D-Bus connection — keeps portal alive

    bool valid() const { return node_id != static_cast<uint32_t>(-1); }

    ~PortalSession();  // sd_bus_unref on destroy
};

// PipeWire screen capture source.
// Connects to a PipeWire portal stream and delivers BGRA/NV12 frames.
class PipeWireCapture final : public pulsar::core::ICaptureSource {
public:
    // Normal constructor: negotiates portal/Mutter ScreenCast automatically.
    explicit PipeWireCapture();

    // Pre-authorized constructor: reuses an existing portal session.
    // The PortalSession must outlive this PipeWireCapture instance.
    explicit PipeWireCapture(const PortalSession& session);

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
    // Called when the stream format is negotiated — stores actual pixel format.
    static void on_param_changed(void* userdata, uint32_t id,
                                 const struct spa_pod* param);

private:
    void event_loop_thread();
    void teardown();

    // xdg-desktop-portal ScreenCast: open a portal session and return the
    // PipeWire node_id of the screen stream (or PW_ID_ANY on failure).
    // On success *bus_out holds the D-Bus connection that MUST stay alive for
    // the portal session to keep delivering frames.
    // Implemented in portal_screencast.cpp — separate from this class.

    // org.gnome.Mutter.ScreenCast: use the compositor's private API directly.
    // Implemented in mutter_screencast.cpp — separate from this class.

    static constexpr int kDefaultWidth  = 1920;
    static constexpr int kDefaultHeight = 1080;
    static constexpr int kDefaultFps    = 60;

    // Actual negotiated frame dimensions (updated when stream format is known).
    int width_  = kDefaultWidth;
    int height_ = kDefaultHeight;
    // Pixel format reported by the compositor (BGRx/BGRA for portals, NV12 for cameras).
    pulsar::core::PixelFormat negotiated_fmt_ = pulsar::core::PixelFormat::NV12;

    pw_main_loop* loop_    = nullptr;
    pw_context*   context_ = nullptr;
    pw_core*      core_    = nullptr;
    pw_stream*    stream_  = nullptr;
    spa_hook      stream_listener_{};

    std::thread               loop_thread_;
    mutable std::mutex        frame_mutex_;
    std::condition_variable   frame_cv_;
    std::optional<pulsar::core::RawFrame> pending_frame_;
    std::atomic<bool>         connected_{false};
    std::atomic<bool>         stop_{false};

    int  display_index_  = 0;
    std::function<void(pulsar::core::CaptureEvent)> event_cb_;

    // Keeps the portal D-Bus session alive when this instance owns it
    // (normal constructor only). Null when using a shared PortalSession.
    sd_bus* portal_bus_  = nullptr;
    bool    owns_portal_ = false;
};

} // namespace pulsar::capture::pipewire
