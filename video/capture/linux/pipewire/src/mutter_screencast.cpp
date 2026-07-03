// video/capture/linux/pipewire/src/mutter_screencast.cpp
// GNOME Mutter ScreenCast implementation.
//
// Uses org.gnome.Mutter.ScreenCast D-Bus API to capture the GNOME desktop
// without requiring a user-interaction permission dialog.
//
// Works when running as the same user as gnome-shell (or root for GDM sessions).
// The GDM login screen actively inhibits ScreenCast for security reasons.
//
// Flow:
//   1. Read DBUS_SESSION_BUS_ADDRESS from gnome-shell's /proc environ
//   2. Connect to that bus
//   3. CreateSession({}) → session_path
//   4. RecordMonitor("", {}) → stream_path
//   5. Start()
//   6. Read PipeWireStreamNodeId property → uint32 node_id

#include "mutter_screencast.h"

#include <pipewire/pipewire.h>  // PW_ID_ANY
#include <systemd/sd-bus.h>

#include <cerrno>
#include <chrono>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <iostream>
#include <string>
#include <thread>
#include <unistd.h>

namespace pulsar::capture::pipewire {

// ─── Internal helpers ─────────────────────────────────────────────────────

static std::string read_proc_env(pid_t pid, const char* var) {
    std::string path = "/proc/" + std::to_string(pid) + "/environ";
    int fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0) return {};
    const std::string prefix = std::string(var) + "=";
    char buf[4096];
    std::string data;
    for (;;) {
        ssize_t n = ::read(fd, buf, sizeof(buf));
        if (n <= 0) break;
        data.append(buf, static_cast<size_t>(n));
    }
    ::close(fd);
    size_t pos = 0;
    while (pos < data.size()) {
        size_t end = data.find('\0', pos);
        if (end == std::string::npos) end = data.size();
        std::string entry = data.substr(pos, end - pos);
        if (entry.rfind(prefix, 0) == 0)
            return entry.substr(prefix.size());
        pos = end + 1;
    }
    return {};
}

// ─── Public API ───────────────────────────────────────────────────────────

std::string find_compositor_bus() {
    DIR* d = ::opendir("/proc");
    if (!d) return {};
    dirent* ent;
    while ((ent = ::readdir(d)) != nullptr) {
        if (ent->d_type != DT_DIR) continue;
        bool numeric = true;
        for (const char* p = ent->d_name; *p; ++p)
            if (*p < '0' || *p > '9') { numeric = false; break; }
        if (!numeric) continue;

        pid_t pid = static_cast<pid_t>(std::strtol(ent->d_name, nullptr, 10));
        std::string comm_path = "/proc/" + std::to_string(pid) + "/comm";
        int cf = ::open(comm_path.c_str(), O_RDONLY);
        if (cf < 0) continue;
        char comm[64] = {};
        ::read(cf, comm, sizeof(comm) - 1);
        ::close(cf);
        if (std::string(comm).rfind("gnome-shell", 0) != 0) continue;

        std::string bus = read_proc_env(pid, "DBUS_SESSION_BUS_ADDRESS");
        if (!bus.empty()) {
            ::closedir(d);
            std::cerr << "[mutter_screencast] compositor bus from PID " << pid << "\n";
            return bus;
        }
    }
    ::closedir(d);
    return {};
}

// ─── Internal: core Mutter ScreenCast logic ──────────────────────────────
// Shared by open_mutter_screencast_user and open_mutter_screencast.

static uint32_t do_mutter_screencast(sd_bus* bus) {
    const char* dest  = "org.gnome.Mutter.ScreenCast";
    const char* obj   = "/org/gnome/Mutter/ScreenCast";
    const char* iface = "org.gnome.Mutter.ScreenCast";
    int r;

    // 1. CreateSession({})
    sd_bus_message* reply = nullptr;
    {
        sd_bus_message* msg = nullptr;
        r = sd_bus_message_new_method_call(bus, &msg, dest, obj, iface, "CreateSession");
        if (r < 0) return PW_ID_ANY;
        sd_bus_message_open_container(msg, 'a', "{sv}");
        sd_bus_message_close_container(msg);
        r = sd_bus_call(bus, msg, 5'000'000, nullptr, &reply);
        sd_bus_message_unref(msg);
    }
    if (r < 0) {
        std::cerr << "[mutter_screencast] CreateSession: " << strerror(-r) << "\n";
        return PW_ID_ANY;
    }
    const char* session_path = nullptr;
    sd_bus_message_read(reply, "o", &session_path);
    const std::string sess(session_path ? session_path : "");
    sd_bus_message_unref(reply);
    if (sess.empty()) return PW_ID_ANY;

    // 2. RecordMonitor("", {}) — primary monitor
    reply = nullptr;
    {
        sd_bus_message* msg = nullptr;
        r = sd_bus_message_new_method_call(bus, &msg, dest, sess.c_str(),
                "org.gnome.Mutter.ScreenCast.Session", "RecordMonitor");
        if (r < 0) return PW_ID_ANY;
        sd_bus_message_append(msg, "s", "");
        sd_bus_message_open_container(msg, 'a', "{sv}");
        sd_bus_message_close_container(msg);
        r = sd_bus_call(bus, msg, 5'000'000, nullptr, &reply);
        sd_bus_message_unref(msg);
    }
    if (r < 0) {
        // Fallback: RecordArea(0,0,1920,1080,{})
        sd_bus_message* msg = nullptr;
        r = sd_bus_message_new_method_call(bus, &msg, dest, sess.c_str(),
                "org.gnome.Mutter.ScreenCast.Session", "RecordArea");
        if (r < 0) return PW_ID_ANY;
        sd_bus_message_append(msg, "iiii", 0, 0, 1920, 1080);
        sd_bus_message_open_container(msg, 'a', "{sv}");
        sd_bus_message_close_container(msg);
        r = sd_bus_call(bus, msg, 5'000'000, nullptr, &reply);
        sd_bus_message_unref(msg);
    }
    if (r < 0) {
        std::cerr << "[mutter_screencast] RecordMonitor: " << strerror(-r) << "\n";
        return PW_ID_ANY;
    }
    const char* stream_path = nullptr;
    sd_bus_message_read(reply, "o", &stream_path);
    const std::string strm(stream_path ? stream_path : "");
    sd_bus_message_unref(reply);
    if (strm.empty()) return PW_ID_ANY;

    // 3. Start()
    {
        sd_bus_message* msg = nullptr;
        r = sd_bus_message_new_method_call(bus, &msg, dest, sess.c_str(),
                "org.gnome.Mutter.ScreenCast.Session", "Start");
        if (r < 0) return PW_ID_ANY;
        r = sd_bus_call(bus, msg, 5'000'000, nullptr, nullptr);
        sd_bus_message_unref(msg);
    }
    if (r < 0) {
        std::cerr << "[mutter_screencast] Start: " << strerror(-r) << "\n";
        return PW_ID_ANY;
    }

    // 4. PipeWireStreamNodeId
    // Give Mutter time to connect the stream to PipeWire.
    // Retry up to 3s in 300ms steps.
    uint32_t node_id = PW_ID_ANY;
    for (int attempt = 0; attempt < 10 && node_id == PW_ID_ANY; ++attempt) {
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        sd_bus_message* r_reply = nullptr;
        int rr = sd_bus_get_property(bus, dest, strm.c_str(),
                                     "org.gnome.Mutter.ScreenCast.Stream",
                                     "PipeWireStreamNodeId",
                                     nullptr, &r_reply, "u");
        if (rr >= 0) {
            uint32_t nid = PW_ID_ANY;
            sd_bus_message_read(r_reply, "u", &nid);
            sd_bus_message_unref(r_reply);
            if (nid != PW_ID_ANY && nid != 0) {
                node_id = nid;
                std::cerr << "[mutter_screencast] node_id=" << node_id
                          << " (attempt " << attempt + 1 << ")\n";
            }
        } else if (attempt == 9) {
            std::cerr << "[mutter_screencast] PipeWireStreamNodeId: "
                      << strerror(-rr) << "\n";
        }
    }
    return node_id;
}

// ─── Public API ───────────────────────────────────────────────────────────

// Use an already-open D-Bus connection (e.g. sd_bus_open_user()).
// The user's session bus has GNOME Shell registered on it when logged in.
// No dialog, no authorization — same mechanism as gnome-remote-desktop.
uint32_t open_mutter_screencast_user(sd_bus* bus) {
    return do_mutter_screencast(bus);
}

uint32_t open_mutter_screencast(const std::string& bus_addr) {
    sd_bus* bus = nullptr;
    int r = sd_bus_new(&bus);
    if (r < 0) { std::cerr << "[mutter_screencast] sd_bus_new failed\n"; return PW_ID_ANY; }
    r = sd_bus_set_address(bus, bus_addr.c_str());
    if (r < 0) { sd_bus_unref(bus); return PW_ID_ANY; }
    r = sd_bus_start(bus);
    if (r < 0) {
        std::cerr << "[mutter_screencast] connect to " << bus_addr
                  << " failed: " << strerror(-r) << "\n";
        sd_bus_unref(bus);
        return PW_ID_ANY;
    }
    const uint32_t node_id = do_mutter_screencast(bus);
    sd_bus_unref(bus);
    return node_id;
}

} // namespace pulsar::capture::pipewire
