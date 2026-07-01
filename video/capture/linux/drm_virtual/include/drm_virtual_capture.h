// capture/drm_virtual/include/capture/drm_virtual/drm_virtual_capture.h
// DRM/KMS Virtual Display Capture — Step 9 Headless support.
//
// Creates a kernel-level virtual display using the VKMS (Virtual Kernel
// Mode Setting) driver.  This allows a Wayland/X11 compositor to see a
// real monitor even on a headless server, and we capture from the DRM
// framebuffer directly via mmap.
//
// Prerequisites:
//   sudo modprobe vkms            # once per boot (or add to /etc/modules)
//
// Usage:
//   DrmVirtualCapture cap;
//   cap.set_resolution(1920, 1080, 60);
//   // Then use as ICaptureSource in the pipeline.
//
// Auto-detection: factory.cpp calls DrmVirtualCapture::is_available() and
// switches to this adapter when no physical display is detected.
#pragma once

#include "capture.h"
#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace pulsar::capture::drm_virtual {

// Forward-declare Impl at namespace scope (avoids private-nested-struct issue).
struct DrmVirtualCaptureImpl;

class DrmVirtualCapture final : public pulsar::core::ICaptureSource {
public:
    DrmVirtualCapture();
    ~DrmVirtualCapture() override;

    // Configure the virtual display resolution before the first next_frame().
    // Defaults to 1920×1080 @ 60 Hz.
    void set_resolution(int width, int height, int fps = 60);

    // Returns true if a VKMS device is present (card0 or another VKMS card).
    static bool is_available();

    // ICaptureSource ─────────────────────────────────────────────────────────
    std::optional<pulsar::core::RawFrame>    next_frame()  override;
    std::optional<pulsar::core::CursorState> next_cursor() override;
    std::vector<pulsar::core::DisplayInfo>   enumerate_displays() const override;
    void select_display(int index) override;
    int  display_refresh_rate() const override;
    void set_event_callback(std::function<void(pulsar::core::CaptureEvent)> cb) override;
    pulsar::core::AdapterCapabilities capabilities() const override;

private:
    bool open_vkms_device();
    bool setup_virtual_display();
    void teardown();
    void read_fb_to_nv12(pulsar::core::RawFrame& frame);

    std::unique_ptr<DrmVirtualCaptureImpl> impl_;
};

} // namespace pulsar::capture::drm_virtual
