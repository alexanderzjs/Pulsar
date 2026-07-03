// video/capture/linux/pipewire/src/mutter_screencast.h
// GNOME Mutter ScreenCast — internal header.
// Provides screen capture via org.gnome.Mutter.ScreenCast D-Bus API.
// Does NOT require a user-interaction dialog — captures immediately.
#pragma once

#include <cstdint>
#include <string>

struct sd_bus;

namespace pulsar::capture::pipewire {

// Scan /proc for a running gnome-shell and return its DBUS_SESSION_BUS_ADDRESS.
// Returns empty string if not found or not readable.
std::string find_compositor_bus();

// Use an already-open D-Bus connection to call Mutter ScreenCast.
// This is the preferred path when the user is logged in:
//   sd_bus_open_user() → connects to /run/user/UID/bus
//   → GNOME Shell registers org.gnome.Mutter.ScreenCast on that bus
//   → No dialog needed, works instantly.
uint32_t open_mutter_screencast_user(sd_bus* bus);

// Connect to a specific bus address and call Mutter ScreenCast.
// Used when running as a different user than gnome-shell (e.g. root / sudo).
uint32_t open_mutter_screencast(const std::string& bus_addr);

} // namespace pulsar::capture::pipewire
