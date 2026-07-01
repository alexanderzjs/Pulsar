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
        // Nested: protocols.rtp.{enabled,port}
        if (j.contains("protocols") && j["protocols"].contains(name)) {
            auto& p = j["protocols"][name];
            if (p.contains("enabled")) ep.enabled = p["enabled"].get<bool>();
            if (p.contains("port"))    ep.port     = p["port"].get<int>();
        }
    };
    load_proto("rtp",    cfg.protocols.rtp);
    load_proto("webrtc", cfg.protocols.webrtc);
    load_proto("quic",   cfg.protocols.quic);
    load_proto("rdp",    cfg.protocols.rdp);
    load_proto("vnc",    cfg.protocols.vnc);

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

    return cfg;
}

} // namespace pulsar::server
