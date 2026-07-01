#pragma once

#include <string>
#include <vector>

namespace pulsar::server {

struct ProtocolEndpointConfig {
    bool enabled = false;
    int  port    = 0;
};

struct ProtocolsConfig {
    ProtocolEndpointConfig rtp    { .enabled = true,  .port = 47984 };
    ProtocolEndpointConfig webrtc { .enabled = false, .port = 47985 };
    ProtocolEndpointConfig quic   { .enabled = false, .port = 47986 };
    ProtocolEndpointConfig rdp    { .enabled = false, .port = 3389  };
    ProtocolEndpointConfig vnc    { .enabled = false, .port = 5900  };
};

struct AuthConfig {
    std::string scheme   = "password";
    std::string username = "admin";
    std::string password = "pulsar";
};

struct CaptureConfig {
    std::string backend = "pipewire";  // "pipewire" | "dxgi" | "sckit"
};

struct EncoderConfig {
    std::string backend     = "vaapi";   // "vaapi" | "nvenc" | "videotoolbox" | "x264"
    int         bitrate_kbps = 8000;
    int         fps          = 60;
};

struct AudioConfig {
    bool        enabled = true;
    std::string capture_backend = "pipewire";  // "pipewire" | "wasapi" | "coreaudio"
    std::string encoder_backend = "opus";
};

struct PipelineConfig {
    int queue_capacity = 4;
    int idle_fps       = 5;
};

struct ProfilerConfig {
    bool        enabled           = true;
    int         benchmark_frames  = 30;
    bool        cache_result      = true;
    int         cache_ttl_seconds = 3600;
    std::string cache_path        = ".pulsar_profile_cache.json";
};

struct ServerConfig {
    AuthConfig      auth{};
    CaptureConfig   capture{};
    EncoderConfig   encoder{};
    AudioConfig     audio{};
    ProtocolsConfig protocols{};
    PipelineConfig  pipeline{};
    ProfilerConfig  profiler{};
};

} // namespace pulsar::server
