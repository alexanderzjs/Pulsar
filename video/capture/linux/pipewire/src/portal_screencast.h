// video/capture/linux/pipewire/src/portal_screencast.h
// xdg-desktop-portal ScreenCast — internal header.
// Universal: works with GNOME (xdg-portal-gnome), KDE (xdg-portal-kde),
// and wlroots compositors (xdg-portal-wlr).
// Requires a logged-in user session and shows a permission dialog.
#pragma once

#include <cstdint>

// Forward-declare sd_bus so we don't pull in systemd headers here.
struct sd_bus;

namespace pulsar::capture::pipewire {

// Result of a successful portal screencast negotiation.
struct PortalScreencastResult {
    uint32_t node_id = static_cast<uint32_t>(-1);  // PipeWire stream node
    int      pw_fd   = -1;   // PipeWire remote fd from OpenPipeWireRemote
    sd_bus*  bus     = nullptr;  // D-Bus connection (must stay alive)

    bool valid() const { return node_id != static_cast<uint32_t>(-1) && pw_fd >= 0; }
};

// Open an xdg-desktop-portal ScreenCast session.
// Blocks until the user grants/denies permission in the system dialog.
// On success:
//   - result.node_id = PipeWire stream node
//   - result.pw_fd   = fd for pw_context_connect_fd() (owned by caller, close() it)
//   - result.bus     = D-Bus connection (caller must sd_bus_unref when done)
// On failure: returns result with valid() == false.
PortalScreencastResult open_portal_screencast();

} // namespace pulsar::capture::pipewire
