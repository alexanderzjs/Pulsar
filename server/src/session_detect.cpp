#include "session_detect.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

namespace pulsar::server {

namespace {

// ─────────────────────────────────────────────────────────────────────────────
// Helpers
// ─────────────────────────────────────────────────────────────────────────────

static std::string uid_str() {
    return std::to_string(static_cast<unsigned long>(::getuid()));
}

static bool file_accessible(const std::string& path) {
    return ::access(path.c_str(), F_OK) == 0;
}

/// Read one NUL-terminated entry from /proc/<pid>/environ that starts with
/// <var>=. Returns the value (after '='), or empty string if not found.
static std::string proc_env_var(pid_t pid, const char* var) {
    const std::string path = "/proc/" + std::to_string(pid) + "/environ";
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) return {};

    // 64 KiB is the practical upper bound for /proc/<pid>/environ
    std::string buf(65536, '\0');
    ssize_t n = ::read(fd, buf.data(), buf.size() - 1);
    ::close(fd);
    if (n <= 0) return {};

    const std::string prefix = std::string(var) + "=";
    const char* p   = buf.data();
    const char* end = p + n;
    while (p < end) {
        if (::strncmp(p, prefix.c_str(), prefix.size()) == 0) {
            return std::string(p + prefix.size());
        }
        // Advance past this NUL-terminated string
        const char* nul = static_cast<const char*>(::memchr(p, '\0', end - p));
        p = nul ? nul + 1 : end;
    }
    return {};
}

/// Return true if /proc/<pid> is owned by the current user.
static bool owned_by_us(pid_t pid) {
    struct stat st{};
    const std::string path = "/proc/" + std::to_string(pid);
    return ::stat(path.c_str(), &st) == 0 && st.st_uid == ::getuid();
}

/// Return true if the process name contains one of the known compositor
/// keywords. We check /proc/<pid>/cmdline (first NUL-terminated token).
static bool is_compositor_process(pid_t pid) {
    const std::string path = "/proc/" + std::to_string(pid) + "/cmdline";
    std::ifstream f(path);
    if (!f) return false;
    std::string cmd;
    std::getline(f, cmd, '\0');
    // Match on basename to avoid false positives from path components
    const auto slash = cmd.rfind('/');
    const std::string name = (slash == std::string::npos) ? cmd : cmd.substr(slash + 1);
    static const char* const kNames[] = {
        "gnome-session", "mutter", "kwin_wayland", "kwin",
        "sway", "gamescope", "cage", "weston", "plasmashell",
        "labwc", "river", "hikari", nullptr
    };
    for (const char* const* n = kNames; *n; ++n) {
        if (name.find(*n) != std::string::npos) return true;
    }
    return false;
}

// ─────────────────────────────────────────────────────────────────────────────
// Strategy 1 – current process environment
// ─────────────────────────────────────────────────────────────────────────────

static WaylandSessionInfo from_environment() {
    const char* xrd  = ::getenv("XDG_RUNTIME_DIR");
    const char* dbus = ::getenv("DBUS_SESSION_BUS_ADDRESS");
    const char* seat = ::getenv("XDG_SEAT");
    const char* wl   = ::getenv("WAYLAND_DISPLAY");
    if (!xrd || !xrd[0] || !wl || !wl[0]) return {};

    WaylandSessionInfo info;
    info.found           = true;
    info.dbus_address    = (dbus && dbus[0]) ? dbus : "";
    info.xdg_runtime_dir = xrd;
    info.xdg_seat        = (seat && seat[0]) ? seat : "seat0";
    info.wayland_display = (wl   && wl[0])   ? wl   : "wayland-0";
    return info;
}

// ─────────────────────────────────────────────────────────────────────────────
// Strategy 2 – standard systemd-logind paths under /run/user/<uid>/
// ─────────────────────────────────────────────────────────────────────────────

static WaylandSessionInfo from_logind_paths() {
    const std::string xrd = "/run/user/" + uid_str();

    // Wayland compositor socket — try wayland-0 then wayland-1
    std::string wl_disp;
    for (const char* candidate : {"wayland-0", "wayland-1"}) {
        if (file_accessible(xrd + "/" + candidate)) {
            wl_disp = candidate;
            break;
        }
    }
    if (wl_disp.empty()) return {};

    WaylandSessionInfo info;
    info.found           = true;
    const std::string bus_path = xrd + "/bus";
    if (file_accessible(bus_path))
        info.dbus_address = "unix:path=" + bus_path;
    info.xdg_runtime_dir = xrd;
    info.xdg_seat        = "seat0";   // logind default; overridden by /proc scan if needed
    info.wayland_display = wl_disp;
    return info;
}

// ─────────────────────────────────────────────────────────────────────────────
// Strategy 3 – /proc scan for a compositor process owned by us
// ─────────────────────────────────────────────────────────────────────────────

static WaylandSessionInfo from_proc_scan() {
    DIR* dir = ::opendir("/proc");
    if (!dir) return {};

    WaylandSessionInfo result;

    dirent* ent;
    while ((ent = ::readdir(dir)) != nullptr) {
        // Only look at purely-numeric entries (PIDs)
        bool numeric = true;
        for (const char* c = ent->d_name; *c; ++c) {
            if (*c < '0' || *c > '9') { numeric = false; break; }
        }
        if (!numeric || ent->d_name[0] == '\0') continue;

        const pid_t pid = static_cast<pid_t>(::strtol(ent->d_name, nullptr, 10));
        if (pid <= 1) continue;
        if (!owned_by_us(pid)) continue;
        if (!is_compositor_process(pid)) continue;

        const std::string dbus = proc_env_var(pid, "DBUS_SESSION_BUS_ADDRESS");
        const std::string xrd  = proc_env_var(pid, "XDG_RUNTIME_DIR");
        const std::string wl   = proc_env_var(pid, "WAYLAND_DISPLAY");
        if (xrd.empty() && wl.empty()) continue;

        result.found           = true;
        result.dbus_address    = dbus;
        result.xdg_runtime_dir = xrd.empty()
                                    ? "/run/user/" + uid_str()
                                    : xrd;
        const std::string seat = proc_env_var(pid, "XDG_SEAT");
        result.xdg_seat        = seat.empty() ? "seat0" : seat;
        result.wayland_display = wl.empty()   ? "wayland-0" : wl;
        break;  // First matching compositor is authoritative
    }

    ::closedir(dir);
    return result;
}

} // anonymous namespace

// ─────────────────────────────────────────────────────────────────────────────
// Public API
// ─────────────────────────────────────────────────────────────────────────────

WaylandSessionInfo detect_wayland_session() {
    // Strategy 1: already in the session
    {
        auto info = from_environment();
        if (info.found) return info;
    }
    // Strategy 2: systemd-logind well-known paths
    {
        auto info = from_logind_paths();
        if (info.found) return info;
    }
    // Strategy 3: /proc scan
    return from_proc_scan();
}

void apply_wayland_session(const WaylandSessionInfo& info) {
    if (!info.found) return;

    // setenv with overwrite=0: don't clobber values already in the environment
    // (e.g. if the user explicitly set them via config or command line).
    if (!info.dbus_address.empty())
        ::setenv("DBUS_SESSION_BUS_ADDRESS", info.dbus_address.c_str(), 0);
    if (!info.xdg_runtime_dir.empty())
        ::setenv("XDG_RUNTIME_DIR", info.xdg_runtime_dir.c_str(), 0);
    if (!info.wayland_display.empty())
        ::setenv("WAYLAND_DISPLAY", info.wayland_display.c_str(), 0);
    // XDG_SEAT is read by UinputHandler at device-creation time; set it so
    // the virtual input devices land on the correct compositor seat.
    if (!info.xdg_seat.empty())
        ::setenv("XDG_SEAT", info.xdg_seat.c_str(), 0);

    std::cerr << "[session] detected wayland session:"
              << " dbus=" << (info.dbus_address.empty() ? "(from env)" : info.dbus_address)
              << " xrd="  << info.xdg_runtime_dir
              << " seat=" << info.xdg_seat
              << " wl="   << info.wayland_display
              << "\n";
}

} // namespace pulsar::server
