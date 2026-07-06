#include "config_parser.h"
#include <nlohmann/json.hpp>
#include <fstream>
#include <iostream>

namespace pulsar::server {

ServerConfig default_config() { return {}; }

std::optional<ServerConfig> load_config(const std::string& path,
                                         std::string& error_msg)
{
    std::ifstream f(path);
    if (!f) { error_msg = "cannot open: " + path; return std::nullopt; }

    nlohmann::json j;
    try {
        f >> j;
    } catch (const std::exception& e) {
        error_msg = std::string("JSON parse error: ") + e.what();
        return std::nullopt;
    }

    ServerConfig cfg;

    // Auth
    if (j.contains("scheme"))   cfg.auth.scheme   = j["scheme"].get<std::string>();
    if (j.contains("username")) cfg.auth.username  = j["username"].get<std::string>();
    if (j.contains("password")) cfg.auth.password  = j["password"].get<std::string>();
    // Nested auth block
    if (j.contains("auth")) {
        auto& a = j["auth"];
        if (a.contains("method"))   cfg.auth.scheme   = a["method"].get<std::string>();
        if (a.contains("username")) cfg.auth.username  = a["username"].get<std::string>();
        if (a.contains("password")) cfg.auth.password  = a["password"].get<std::string>();
    }

    // Capture
    if (j.contains("capture_backend")) cfg.capture.backend = j["capture_backend"].get<std::string>();
    if (j.contains("capture") && j["capture"].contains("backend"))
        cfg.capture.backend = j["capture"]["backend"].get<std::string>();
    if (j.contains("capture") && j["capture"].contains("xcb_display"))
        cfg.capture.xcb_display = j["capture"]["xcb_display"].get<std::string>();
    if (j.contains("capture") && j["capture"].contains("xcb_xauthority"))
        cfg.capture.xcb_xauthority = j["capture"]["xcb_xauthority"].get<std::string>();
    if (j.contains("capture") && j["capture"].contains("dbus_address"))
        cfg.capture.dbus_address = j["capture"]["dbus_address"].get<std::string>();
    if (j.contains("capture") && j["capture"].contains("xdg_runtime_dir"))
        cfg.capture.xdg_runtime_dir = j["capture"]["xdg_runtime_dir"].get<std::string>();

    // Compositor host
    if (j.contains("compositor")) {
        auto& c = j["compositor"];
        if (c.contains("enabled")) cfg.compositor.enabled = c["enabled"].get<bool>();
        if (c.contains("self_host_session")) cfg.compositor.self_host_session = c["self_host_session"].get<bool>();
        if (c.contains("start_pipewire_pulse")) cfg.compositor.start_pipewire_pulse = c["start_pipewire_pulse"].get<bool>();
        if (c.contains("command")) cfg.compositor.command = c["command"].get<std::string>();
        if (c.contains("runtime_dir")) cfg.compositor.runtime_dir = c["runtime_dir"].get<std::string>();
        if (c.contains("wayland_display")) cfg.compositor.wayland_display = c["wayland_display"].get<std::string>();
        if (c.contains("xdg_seat")) cfg.compositor.xdg_seat = c["xdg_seat"].get<std::string>();
        if (c.contains("startup_timeout_ms")) cfg.compositor.startup_timeout_ms = c["startup_timeout_ms"].get<int>();
        if (c.contains("args") && c["args"].is_array()) {
            cfg.compositor.args.clear();
            for (const auto& arg : c["args"])
                cfg.compositor.args.push_back(arg.get<std::string>());
        }
        if (c.contains("autostart") && c["autostart"].is_array()) {
            cfg.compositor.autostart.clear();
            for (const auto& app : c["autostart"])
                cfg.compositor.autostart.push_back(app.get<std::string>());
        }
    }

    // Encoder
    if (j.contains("encoder_backend")) cfg.encoder.backend      = j["encoder_backend"].get<std::string>();
    if (j.contains("bitrate_kbps"))    cfg.encoder.bitrate_kbps = j["bitrate_kbps"].get<int>();
    if (j.contains("fps"))             cfg.encoder.fps           = j["fps"].get<int>();
    if (j.contains("encoder")) {
        auto& e = j["encoder"];
        if (e.contains("backend"))     cfg.encoder.backend      = e["backend"].get<std::string>();
        if (e.contains("bitrate_kbps"))cfg.encoder.bitrate_kbps = e["bitrate_kbps"].get<int>();
        if (e.contains("fps"))         cfg.encoder.fps          = e["fps"].get<int>();
        if (e.contains("codec"))       /* future field, ignored for now */ ;
    }

    // Audio
    if (j.contains("audio_enabled")) cfg.audio.enabled = j["audio_enabled"].get<bool>();
    if (j.contains("audio")) {
        auto& a = j["audio"];
        if (a.contains("enabled"))         cfg.audio.enabled         = a["enabled"].get<bool>();
        if (a.contains("capture_backend")) cfg.audio.capture_backend = a["capture_backend"].get<std::string>();
        if (a.contains("encoder"))         cfg.audio.encoder_backend = a["encoder"].get<std::string>();
    }

    // Protocols
    auto load_proto = [&](const std::string& name, ProtocolEndpointConfig& ep) {
        // Flat keys (legacy): rtp_enabled, rtp_port
        auto en_key   = name + "_enabled";
        auto port_key = name + "_port";
        if (j.contains(en_key))   ep.enabled = j[en_key].get<bool>();
        if (j.contains(port_key)) ep.port     = j[port_key].get<int>();
        // Nested: protocols.rtp.{enabled,port,push_to}
        if (j.contains("protocols") && j["protocols"].contains(name)) {
            auto& p = j["protocols"][name];
            if (p.contains("enabled")) ep.enabled = p["enabled"].get<bool>();
            if (p.contains("port"))    ep.port     = p["port"].get<int>();
            if (p.contains("push_to")) ep.push_to  = p["push_to"].get<std::string>();
        }
    };
    load_proto("rtp",          cfg.protocols.rtp);
    load_proto("webrtc",       cfg.protocols.webrtc);
    load_proto("quic",         cfg.protocols.quic);
    load_proto("webtransport", cfg.protocols.webtransport);

    // Pipeline
    if (j.contains("queue_capacity")) cfg.pipeline.queue_capacity = j["queue_capacity"].get<int>();
    if (j.contains("idle_fps"))       cfg.pipeline.idle_fps       = j["idle_fps"].get<int>();
    if (j.contains("pipeline")) {
        auto& p = j["pipeline"];
        if (p.contains("queue_capacity")) cfg.pipeline.queue_capacity = p["queue_capacity"].get<int>();
        if (p.contains("idle_fps"))       cfg.pipeline.idle_fps       = p["idle_fps"].get<int>();
    }

    // Profiler
    if (j.contains("profiler_enabled")) cfg.profiler.enabled          = j["profiler_enabled"].get<bool>();
    if (j.contains("profiler_frames"))  cfg.profiler.benchmark_frames = j["profiler_frames"].get<int>();
    if (j.contains("profiler_cache"))   cfg.profiler.cache_result     = j["profiler_cache"].get<bool>();
    if (j.contains("profiler")) {
        auto& p = j["profiler"];
        if (p.contains("enabled"))          cfg.profiler.enabled          = p["enabled"].get<bool>();
        if (p.contains("benchmark_frames")) cfg.profiler.benchmark_frames = p["benchmark_frames"].get<int>();
        if (p.contains("cache_result"))     cfg.profiler.cache_result     = p["cache_result"].get<bool>();
        if (p.contains("cache_ttl_seconds"))cfg.profiler.cache_ttl_seconds= p["cache_ttl_seconds"].get<int>();
    }

    // Recording
    if (j.contains("recording")) {
        auto& r = j["recording"];
        if (r.contains("enabled"))              cfg.recording.enabled             = r["enabled"].get<bool>();
        if (r.contains("output_dir"))           cfg.recording.output_dir          = r["output_dir"].get<std::string>();
        if (r.contains("max_duration_minutes")) cfg.recording.max_duration_minutes= r["max_duration_minutes"].get<int>();
    }

    // Wake-on-LAN
    if (j.contains("wake_on_lan")) {
        auto& w = j["wake_on_lan"];
        if (w.contains("enabled"))           cfg.wake_on_lan.enabled           = w["enabled"].get<bool>();
        if (w.contains("listen_port"))       cfg.wake_on_lan.listen_port       = w["listen_port"].get<int>();
        if (w.contains("broadcast_address")) cfg.wake_on_lan.broadcast_address = w["broadcast_address"].get<std::string>();
    }

    // Shared session
    if (j.contains("shared_session")) {
        auto& s = j["shared_session"];
        if (s.contains("enabled"))     cfg.shared_session.enabled     = s["enabled"].get<bool>();
        if (s.contains("max_clients")) cfg.shared_session.max_clients = s["max_clients"].get<int>();
    }

    // Apps
    if (j.contains("apps") && j["apps"].is_array()) {
        for (const auto& a : j["apps"]) {
            AppEntry e;
            if (a.contains("id"))          e.id          = a["id"].get<std::string>();
            if (a.contains("name"))        e.name        = a["name"].get<std::string>();
            if (a.contains("executable"))  e.executable  = a["executable"].get<std::string>();
            if (a.contains("args"))        e.args        = a["args"].get<std::string>();
            if (a.contains("working_dir")) e.working_dir = a["working_dir"].get<std::string>();
            if (!e.id.empty()) cfg.apps.push_back(std::move(e));
        }
    }

    return cfg;
}

} // namespace pulsar::server
