// video/capture/linux/pipewire/src/kwin_screencast.h
// KDE KWin ScreenCast — internal header.
// TODO: Implement using org.kde.KWin.ScreenCast D-Bus API.
// Works similarly to Mutter ScreenCast but KWin-specific.
#pragma once

#include <cstdint>
#include <string>

namespace pulsar::capture::pipewire {

// Returns PipeWire node_id from KWin ScreenCast, or PW_ID_ANY on failure.
// Currently unimplemented — always returns PW_ID_ANY.
inline uint32_t open_kwin_screencast(const std::string& /*bus_addr*/) {
    return PW_ID_ANY;  // TODO: implement org.kde.KWin.ScreenCast
}

} // namespace pulsar::capture::pipewire
