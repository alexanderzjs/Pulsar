#pragma once
#include "capture.h"
#include <atomic>
#include <memory>
#include <string>

namespace pulsar::capture::xcb {

// Captures frames from an X11 display using XCB + SHM.
// Connect to DISPLAY (default :99 / Xvfb) and return BGRA RawFrames.
// Usage: set DISPLAY=:99 on the server, launch games there, stream.
class XcbCapture final : public pulsar::core::ICaptureSource {
public:
    // display_name: e.g. ":99" (Xvfb) or ":0" (existing desktop)
    explicit XcbCapture(const std::string& display_name = ":99");
    ~XcbCapture() override;

    std::optional<pulsar::core::RawFrame> next_frame() override;
    std::optional<pulsar::core::CursorState> next_cursor() override;
    std::vector<pulsar::core::DisplayInfo> enumerate_displays() const override;
    void select_display(int index) override;
    int  display_refresh_rate() const override;
    void set_event_callback(std::function<void(pulsar::core::CaptureEvent)> cb) override;
    pulsar::core::AdapterCapabilities capabilities() const override;

    bool is_open() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace pulsar::capture::xcb
