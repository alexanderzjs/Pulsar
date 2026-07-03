// video/capture/linux/pipewire/src/portal_screencast.cpp
// xdg-desktop-portal ScreenCast implementation.
//
// Universal: works with any compositor that has an xdg-portal backend:
//   GNOME  → xdg-desktop-portal-gnome
//   KDE    → xdg-desktop-portal-kde
//   sway   → xdg-desktop-portal-wlr
//   Hyprland → xdg-desktop-portal-hyprland
//
// Requires a logged-in user session and shows the compositor's screen-share
// permission dialog on first use.
//
// The returned D-Bus bus connection MUST stay alive for the portal session
// to keep delivering PipeWire frames (do not sd_bus_unref until capture ends).

#include "portal_screencast.h"

#include <pipewire/pipewire.h>  // PW_ID_ANY
#include <systemd/sd-bus.h>

#include <cerrno>
#include <chrono>
#include <cstring>
#include <iostream>
#include <string>
#include <unistd.h>

namespace pulsar::capture::pipewire {

// ─── Internal helpers ─────────────────────────────────────────────────────

static std::string make_token(const char* prefix) {
    return std::string(prefix) + "_" + std::to_string(::getpid());
}

// Poll the bus for the portal Response signal on `request_path`.
// Returns 0 (granted) or non-zero (denied/timeout).
// On success, `node_id_out` is set from the `streams` result.
static int wait_for_portal_response(sd_bus* bus,
                                     const std::string& request_path,
                                     uint32_t& node_id_out,
                                     int timeout_sec = 60) {
    sd_bus_message* reply = nullptr;
    uint32_t response = 1;
    node_id_out = PW_ID_ANY;

    const auto deadline = std::chrono::steady_clock::now()
                        + std::chrono::seconds(timeout_sec);

    while (std::chrono::steady_clock::now() < deadline) {
        int r = sd_bus_process(bus, &reply);
        if (r < 0) break;
        if (r == 0) { sd_bus_wait(bus, 100'000); continue; }
        if (!reply) continue;

        if (sd_bus_message_is_signal(reply,
                "org.freedesktop.portal.Request", "Response") &&
            std::string(sd_bus_message_get_path(reply)) == request_path) {

            uint32_t resp = 1;
            sd_bus_message_read(reply, "u", &resp);
            response = resp;

            if (resp == 0) {
                sd_bus_message_enter_container(reply, 'a', "{sv}");
                const char* key = nullptr;
                while (sd_bus_message_enter_container(reply, 'e', "sv") > 0) {
                    sd_bus_message_read(reply, "s", &key);
                    if (key && std::string(key) == "streams") {
                        sd_bus_message_enter_container(reply, 'v', "a(ua{sv})");
                        sd_bus_message_enter_container(reply, 'a', "(ua{sv})");
                        if (sd_bus_message_enter_container(reply, 'r', "ua{sv}") > 0) {
                            uint32_t nid = PW_ID_ANY;
                            sd_bus_message_read(reply, "u", &nid);
                            node_id_out = nid;
                            sd_bus_message_exit_container(reply);
                        }
                        sd_bus_message_exit_container(reply);
                        sd_bus_message_exit_container(reply);
                    } else {
                        sd_bus_message_skip(reply, "v");
                    }
                    sd_bus_message_exit_container(reply);
                }
            }
            sd_bus_message_unref(reply);
            return static_cast<int>(response);
        }
        sd_bus_message_unref(reply);
        reply = nullptr;
    }
    return 1; // timed out
}

// ─── Public API ───────────────────────────────────────────────────────────

PortalScreencastResult open_portal_screencast() {

    sd_bus* bus = nullptr;
    int r = sd_bus_open_user(&bus);
    if (r < 0) {
        std::cerr << "[portal_screencast] sd_bus_open_user failed: "
                  << strerror(-r) << "\n";
        return {};
    }

    // ── 1. CreateSession ──────────────────────────────────────────────────
    const std::string sess_token = make_token("pulsar_sess");
    std::string session_handle;
    {
        sd_bus_message* msg = nullptr;
        r = sd_bus_message_new_method_call(bus, &msg,
                "org.freedesktop.portal.Desktop",
                "/org/freedesktop/portal/desktop",
                "org.freedesktop.portal.ScreenCast", "CreateSession");
        if (r < 0) { sd_bus_unref(bus); return {}; }

        sd_bus_message_open_container(msg, 'a', "{sv}");
        sd_bus_message_open_container(msg, 'e', "sv");
        sd_bus_message_append(msg, "s", "handle_token");
        sd_bus_message_open_container(msg, 'v', "s");
        sd_bus_message_append(msg, "s", make_token("pulsar_req").c_str());
        sd_bus_message_close_container(msg);
        sd_bus_message_close_container(msg);
        sd_bus_message_open_container(msg, 'e', "sv");
        sd_bus_message_append(msg, "s", "session_handle_token");
        sd_bus_message_open_container(msg, 'v', "s");
        sd_bus_message_append(msg, "s", sess_token.c_str());
        sd_bus_message_close_container(msg);
        sd_bus_message_close_container(msg);
        sd_bus_message_close_container(msg);

        sd_bus_message* reply = nullptr;
        r = sd_bus_call(bus, msg, 0, nullptr, &reply);
        sd_bus_message_unref(msg);
        if (r < 0) { if (reply) sd_bus_message_unref(reply); sd_bus_unref(bus); return {}; }

        const char* req_path = nullptr;
        sd_bus_message_read(reply, "o", &req_path);
        std::string request_path = req_path ? req_path : "";
        sd_bus_message_unref(reply);

        uint32_t dummy = PW_ID_ANY;
        if (wait_for_portal_response(bus, request_path, dummy) != 0) {
            std::cerr << "[portal_screencast] CreateSession denied\n";
            sd_bus_unref(bus);
            return {};
        }

        const char* s = nullptr;
        sd_bus_get_unique_name(bus, &s);
        if (s) {
            std::string sname(s);
            if (sname.size() > 1 && sname[0] == ':') sname = sname.substr(1);
            for (char& c : sname) if (c == '.' || c == ':') c = '_';
            session_handle = "/org/freedesktop/portal/desktop/session/"
                           + sname + "/" + sess_token;
        }
    }
    if (session_handle.empty()) { sd_bus_unref(bus); return {}; }
    std::cerr << "[portal_screencast] session: " << session_handle << "\n";

    // ── 2. SelectSources ──────────────────────────────────────────────────
    {
        sd_bus_message* msg = nullptr;
        r = sd_bus_message_new_method_call(bus, &msg,
                "org.freedesktop.portal.Desktop",
                "/org/freedesktop/portal/desktop",
                "org.freedesktop.portal.ScreenCast", "SelectSources");
        if (r < 0) { sd_bus_unref(bus); return {}; }

        sd_bus_message_append(msg, "o", session_handle.c_str());
        sd_bus_message_open_container(msg, 'a', "{sv}");
        // types: 1=MONITOR
        sd_bus_message_open_container(msg, 'e', "sv");
        sd_bus_message_append(msg, "s", "types");
        sd_bus_message_open_container(msg, 'v', "u");
        sd_bus_message_append(msg, "u", (uint32_t)1);
        sd_bus_message_close_container(msg); sd_bus_message_close_container(msg);
        // multiple: false
        sd_bus_message_open_container(msg, 'e', "sv");
        sd_bus_message_append(msg, "s", "multiple");
        sd_bus_message_open_container(msg, 'v', "b");
        sd_bus_message_append(msg, "b", 0);
        sd_bus_message_close_container(msg); sd_bus_message_close_container(msg);
        // handle_token
        sd_bus_message_open_container(msg, 'e', "sv");
        sd_bus_message_append(msg, "s", "handle_token");
        sd_bus_message_open_container(msg, 'v', "s");
        sd_bus_message_append(msg, "s", make_token("pulsar_sel").c_str());
        sd_bus_message_close_container(msg); sd_bus_message_close_container(msg);
        sd_bus_message_close_container(msg);

        sd_bus_message* reply = nullptr;
        r = sd_bus_call(bus, msg, 0, nullptr, &reply);
        sd_bus_message_unref(msg);
        if (r < 0) { if (reply) sd_bus_message_unref(reply); sd_bus_unref(bus); return {}; }
        const char* rp = nullptr;
        std::string request_path;
        sd_bus_message_read(reply, "o", &rp);
        if (rp) request_path = rp;
        sd_bus_message_unref(reply);

        uint32_t dummy = PW_ID_ANY;
        if (wait_for_portal_response(bus, request_path, dummy) != 0) {
            std::cerr << "[portal_screencast] SelectSources denied\n";
            sd_bus_unref(bus);
            return {};
        }
    }

    // ── 3. Start (shows permission dialog) ───────────────────────────────
    uint32_t node_id = PW_ID_ANY;
    {
        sd_bus_message* msg = nullptr;
        r = sd_bus_message_new_method_call(bus, &msg,
                "org.freedesktop.portal.Desktop",
                "/org/freedesktop/portal/desktop",
                "org.freedesktop.portal.ScreenCast", "Start");
        if (r < 0) { sd_bus_unref(bus); return {}; }

        sd_bus_message_append(msg, "os", session_handle.c_str(), "");
        sd_bus_message_open_container(msg, 'a', "{sv}");
        sd_bus_message_open_container(msg, 'e', "sv");
        sd_bus_message_append(msg, "s", "handle_token");
        sd_bus_message_open_container(msg, 'v', "s");
        sd_bus_message_append(msg, "s", make_token("pulsar_start").c_str());
        sd_bus_message_close_container(msg); sd_bus_message_close_container(msg);
        sd_bus_message_close_container(msg);

        sd_bus_message* reply = nullptr;
        r = sd_bus_call(bus, msg, 0, nullptr, &reply);
        sd_bus_message_unref(msg);
        if (r < 0) { if (reply) sd_bus_message_unref(reply); sd_bus_unref(bus); return {}; }
        const char* rp = nullptr;
        std::string request_path;
        sd_bus_message_read(reply, "o", &rp);
        if (rp) request_path = rp;
        sd_bus_message_unref(reply);

        std::cerr << "[portal_screencast] waiting for permission...\n";
        if (wait_for_portal_response(bus, request_path, node_id, 120) != 0) {
            std::cerr << "[portal_screencast] Start denied or timed out\n";
            sd_bus_unref(bus);
            return {};
        }
    }

    if (node_id == PW_ID_ANY) {
        std::cerr << "[portal_screencast] no streams in response\n";
        sd_bus_unref(bus);
        return {};
    }
    std::cerr << "[portal_screencast] granted node_id=" << node_id << "\n";

    // ── 4. OpenPipeWireRemote ─────────────────────────────────────────────
    // Returns an authenticated PipeWire fd. MUST use pw_context_connect_fd()
    // with this fd instead of the default socket — without it the stream
    // connects to a context that has no permission to read portal nodes and
    // stays stuck in PAUSED forever (never enters STREAMING state).
    int pw_fd = -1;
    {
        sd_bus_message* msg = nullptr;
        r = sd_bus_message_new_method_call(bus, &msg,
                "org.freedesktop.portal.Desktop",
                "/org/freedesktop/portal/desktop",
                "org.freedesktop.portal.ScreenCast", "OpenPipeWireRemote");
        if (r >= 0) {
            sd_bus_message_append(msg, "o", session_handle.c_str());
            // empty options dict
            sd_bus_message_open_container(msg, 'a', "{sv}");
            sd_bus_message_close_container(msg);

            sd_bus_message* reply = nullptr;
            r = sd_bus_call(bus, msg, 0, nullptr, &reply);
            sd_bus_message_unref(msg);
            if (r >= 0 && reply) {
                int fd = -1;
                if (sd_bus_message_read(reply, "h", &fd) >= 0 && fd >= 0)
                    pw_fd = dup(fd);   // dup — we own it after reply freed
                sd_bus_message_unref(reply);
            }
        }
    }
    if (pw_fd < 0) {
        std::cerr << "[portal_screencast] OpenPipeWireRemote failed\n";
        sd_bus_unref(bus);
        return {};
    }
    std::cerr << "[portal_screencast] pw_fd=" << pw_fd << "\n";

    // bus must stay alive — portal session closes when bus is destroyed.
    PortalScreencastResult res;
    res.node_id = node_id;
    res.pw_fd   = pw_fd;
    res.bus     = bus;   // caller owns — call sd_bus_unref when capture ends
    return res;
}

} // namespace pulsar::capture::pipewire
