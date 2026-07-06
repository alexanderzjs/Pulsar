#pragma once

#include <string>

namespace pulsar::server {

/// Environment snapshot of a running Wayland compositor session.
struct WaylandSessionInfo {
    bool        found           = false;
    std::string dbus_address;     ///< DBUS_SESSION_BUS_ADDRESS
    std::string xdg_runtime_dir;  ///< XDG_RUNTIME_DIR (e.g. /run/user/1001)
    std::string xdg_seat;         ///< XDG_SEAT (e.g. "seat0")
    std::string wayland_display;  ///< WAYLAND_DISPLAY (e.g. "wayland-0")
};

/// Detect the Wayland session for the current user without requiring the
/// caller to already be inside a graphical environment.
///
/// Detection order:
///   1. Current process environment — if DBUS_SESSION_BUS_ADDRESS and
///      XDG_RUNTIME_DIR are already set (e.g. launched from a terminal
///      inside the session), return them immediately.
///   2. systemd-logind standard paths — /run/user/<uid>/bus (D-Bus socket)
///      and /run/user/<uid>/wayland-{0,1} (Wayland socket). This is the
///      expected headless path: the compositor is running as a systemd user
///      service and everything is in the well-known XDG_RUNTIME_DIR.
///   3. /proc scan — iterate /proc/<pid>/environ for processes owned by the
///      current user that look like a compositor (gnome-session, mutter,
///      kwin_wayland, sway, gamescope, cage, weston). Extract their env vars.
///
/// Returns info.found == false when no Wayland session can be located.
WaylandSessionInfo detect_wayland_session();

/// Apply a detected session to the process environment so that subsequent
/// calls to setenv / getenv by libraries (PipeWire, GLib) pick them up.
/// Only sets variables that are not already present in the environment.
/// Also sets XDG_SEAT when xdg_seat is non-empty (required by UinputHandler).
void apply_wayland_session(const WaylandSessionInfo& info);

} // namespace pulsar::server
