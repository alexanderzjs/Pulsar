#pragma once
#include "capture.h"

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace pulsar::capture::pipewire {

// Captures frames from a GNOME Wayland session via xdg-desktop-portal and
// PipeWire ScreenCast.
// Requires: DBUS_SESSION_BUS_ADDRESS, XDG_RUNTIME_DIR set before construction.
class PipeWireCapture final : public pulsar::core::ICaptureSource {
public:
    explicit PipeWireCapture(const std::string& dbus_address    = "",
                              const std::string& xdg_runtime_dir = "");
    ~PipeWireCapture() override;

    bool is_open() const;

    // Discard any frames queued from a previous session.
    // Call this before run_pipeline_for_session to avoid stale frames.
    void flush_queue();

    // ICaptureSource
    std::optional<pulsar::core::RawFrame>    next_frame()  override;
    std::optional<pulsar::core::CursorState> next_cursor() override;
    std::vector<pulsar::core::DisplayInfo>   enumerate_displays() const override;
    void select_display(int id)   override;
    int  display_refresh_rate()   const override;
    void set_event_callback(std::function<void(pulsar::core::CaptureEvent)>) override;
    pulsar::core::AdapterCapabilities capabilities() const override;

    // Internal use by static C callbacks
    struct Impl;
private:
    std::unique_ptr<Impl> impl_;
};

} // namespace pulsar::capture::pipewire
