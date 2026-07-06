#pragma once

#include <string>
#include <vector>

namespace pulsar::server {

struct ProtocolEndpointConfig {
    bool        enabled = false;
    int         port    = 0;
    std::string push_to = ""; // If set, skip hello; push directly to this IP
};

struct ProtocolsConfig {
    ProtocolEndpointConfig rtp          { .enabled = true,  .port = 47984 };
    ProtocolEndpointConfig webrtc       { .enabled = false, .port = 47985 };
    ProtocolEndpointConfig quic         { .enabled = false, .port = 47986 };
    ProtocolEndpointConfig webtransport { .enabled = false, .port = 47987 };
    // RDP / VNC are optional compatibility adapters, disabled by default
    ProtocolEndpointConfig rdp          { .enabled = false, .port = 3389  };
    ProtocolEndpointConfig vnc          { .enabled = false, .port = 5900  };
};

struct AuthConfig {
    std::string scheme   = "password";
    std::string username = "admin";
    std::string password = "pulsar";
};

struct CaptureConfig {
    std::string backend       = "drm_virtual";  // "drm_virtual" | "xcb" | "pipewire" | "synthetic"
    std::string xcb_display   = ":99";          // X11 display for xcb backend
    std::string xcb_xauthority;                 // XAUTHORITY file (optional, for Xwayland)
    std::string dbus_address;                   // D-Bus session bus address (pipewire backend)
    std::string xdg_runtime_dir;                // XDG_RUNTIME_DIR (pipewire backend)
};

struct CompositorConfig {
    bool        enabled          = false;
    bool        self_host_session = true;       // launch an isolated session bus + PipeWire stack
    bool        start_pipewire_pulse = false;   // optional Pulse compatibility daemon
    std::string command;                       // compositor binary, e.g. weston
    std::vector<std::string> args;             // argv[1..]
    std::vector<std::string> autostart;         // desktop apps to launch after the shell starts
    std::string runtime_dir;                   // optional runtime dir override
    std::string wayland_display  = "wayland-0";
    std::string xdg_seat         = "seat0";
    int         startup_timeout_ms = 5000;
};

struct EncoderConfig {
    std::string backend      = "vaapi";
    int         bitrate_kbps = 8000;
    int         fps          = 60;
};

struct AudioConfig {
    bool        enabled         = true;
    std::string capture_backend = "pipewire";
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

struct RecordingConfig {
    bool        enabled             = false;
    std::string output_dir          = "recordings/";
    int         max_duration_minutes = 120;
};

struct WakeOnLanConfig {
    bool        enabled           = false;
    int         listen_port       = 9;
    std::string broadcast_address = "255.255.255.255";
};

struct AppEntry {
    std::string id;
    std::string name;
    std::string executable;
    std::string args;
    std::string working_dir;
};

struct SharedSessionConfig {
    bool enabled     = false;
    int  max_clients = 4;
};

struct ServerConfig {
    AuthConfig          auth{};
    CaptureConfig       capture{};
    CompositorConfig    compositor{};
    EncoderConfig       encoder{};
    AudioConfig         audio{};
    ProtocolsConfig     protocols{};
    PipelineConfig      pipeline{};
    ProfilerConfig      profiler{};
    RecordingConfig     recording{};
    WakeOnLanConfig     wake_on_lan{};
    SharedSessionConfig shared_session{};
    std::vector<AppEntry> apps{};
};

} // namespace pulsar::server
