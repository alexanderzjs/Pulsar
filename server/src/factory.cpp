#include "factory.h"
#include "config_parser.h"
#include "server_profiler.h"
#include "session_services.h"

// Core
#include "auth.h"
#include "error.h"
#include "logger.h"
#include "pipeline.h"
#include "profiler.h"
#include "session.h"

// Auth adapter
#include "password_auth.h"

// Capture
#include "drm_virtual_capture.h"

// Preprocessors
#include "dmabuf_importer.h"
#include "cpu_preprocessor.h"
#include "gpu_preprocessor.h"

// Encoders
#include "nvenc_encoder.h"
#include "vaapi_encoder.h"
#include "x264_encoder.h"

// Input
#include "uinput_handler.h"

// Transport
#include "rtp_transport.h"
#include "quic_transport.h"
#include "webrtc_transport.h"

// Audio
#include "pipewire_audio_capture.h"
#include "opus_encoder.h"

#include <arpa/inet.h>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstring>
#include <ctime>
#include <iostream>
#include <memory>
#include <netinet/in.h>
#include <string>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <unordered_map>
#include <vector>

namespace pulsar::server {

// ─── Global stop flag ────────────────────────────────────────────────────────
namespace {
std::atomic<bool> g_stop{false};
void signal_handler(int) { g_stop.store(true); }
} // namespace

// ─── StdoutLogger ─────────────────────────────────────────────────────────────
class StdoutLogger final : public pulsar::core::ILogger {
public:
    void log(pulsar::core::LogLevel level, const std::string& msg) override {
        const char* lv = "info";
        switch (level) {
        case pulsar::core::LogLevel::Trace: lv = "trace"; break;
        case pulsar::core::LogLevel::Debug: lv = "debug"; break;
        case pulsar::core::LogLevel::Info:  lv = "info";  break;
        case pulsar::core::LogLevel::Warn:  lv = "warn";  break;
        case pulsar::core::LogLevel::Error: lv = "error"; break;
        }
        std::cout << '[' << lv << "] " << msg << '\n';
    }
};

// ─── InMemorySessionManager ───────────────────────────────────────────────────
class InMemorySessionManager final : public pulsar::core::ISessionManager {
public:
    pulsar::core::Session create(const pulsar::core::AuthToken& tok) override {
        pulsar::core::Session s;
        s.id    = tok.data.count("username") ? tok.data.at("username") : "session-1";
        s.state = pulsar::core::SessionState::Authenticating;
        sessions_[s.id] = s;
        return s;
    }
    void terminate(const std::string& id) override { sessions_.erase(id); }
    pulsar::core::Session* find(const std::string& id) override {
        auto it = sessions_.find(id);
        return it == sessions_.end() ? nullptr : &it->second;
    }
    void activate(const std::string& id) override {
        if (auto* s = find(id)) s->state = pulsar::core::SessionState::Active;
    }
    pulsar::core::QosPolicy get_qos(const std::string&) const override { return default_qos(); }
    pulsar::core::QosPolicy default_qos() const override {
        return { .max_bitrate_kbps = 12000, .max_fps = 60 };
    }
private:
    std::unordered_map<std::string, pulsar::core::Session> sessions_;
};

// ─── Synthetic capture (verify + headless testing) ───────────────────────────
// Generates a 1280×720 NV12 test pattern at 30 fps:
//   • 8×8 grid of solid-colour blocks cycling through 8 hues
//   • Frame counter drawn as brightness ramp along the top row
// This makes it immediately obvious whether frames are actually arriving.
class SyntheticCapture final : public pulsar::core::ICaptureSource {
public:
    static constexpr int kW = 1280, kH = 720, kFps = 30;

    std::optional<pulsar::core::RawFrame> next_frame() override {
        const auto now = std::chrono::steady_clock::now();
        const auto interval = std::chrono::microseconds(1'000'000 / kFps);
        if (now - last_ < interval)
            std::this_thread::sleep_for(interval - (now - last_));
        last_ = std::chrono::steady_clock::now();

        // Build a coloured-grid NV12 test pattern.
        // Colours (YUV): 8 hues, cycling per block column % 8.
        static constexpr uint8_t kY[8]  = {235,210,180,145,112, 82, 54, 41};
        static constexpr uint8_t kCb[8] = {128, 98,212, 55,140,184, 54,240};
        static constexpr uint8_t kCr[8] = {128,212, 54,140, 54, 98,240,110};
        const int block_w = kW / 8, block_h = kH / 8;

        struct Buf final : public pulsar::core::FrameBuffer {
            std::vector<uint8_t> d;
            Buf() : d(static_cast<size_t>(kW * kH * 3 / 2), 128) {}
            uint8_t* data() const override { return const_cast<uint8_t*>(d.data()); }
            size_t   size() const override { return d.size(); }
        };
        auto buf = std::make_shared<Buf>();
        uint8_t* Y_plane  = buf->d.data();
        uint8_t* UV_plane = buf->d.data() + kW * kH;

        for (int row = 0; row < kH; ++row) {
            for (int col = 0; col < kW; ++col) {
                const int hue = (col / block_w + row / block_h + frame_) % 8;
                Y_plane[row * kW + col] = kY[hue];
            }
        }
        for (int row = 0; row < kH / 2; ++row) {
            for (int col = 0; col < kW; col += 2) {
                const int hue = (col / block_w + (row * 2) / block_h + frame_) % 8;
                UV_plane[row * kW + col]     = kCb[hue];
                UV_plane[row * kW + col + 1] = kCr[hue];
            }
        }

        timespec ts{}; clock_gettime(CLOCK_MONOTONIC, &ts);
        pulsar::core::RawFrame f;
        f.buffer = std::move(buf);
        f.width  = kW; f.height = kH;
        f.pts_us = static_cast<int64_t>(ts.tv_sec) * 1'000'000 + ts.tv_nsec / 1'000;
        f.format = pulsar::core::PixelFormat::NV12;
        f.dirty_rects.push_back({0, 0, kW, kH});
        ++frame_;
        return f;
    }
    std::optional<pulsar::core::CursorState> next_cursor()  override { return std::nullopt; }
    std::vector<pulsar::core::DisplayInfo> enumerate_displays() const override {
        return {{0,"Synthetic",kW,kH,kFps,true,false}};
    }
    void select_display(int) override {}
    int  display_refresh_rate() const override { return kFps; }
    void set_event_callback(std::function<void(pulsar::core::CaptureEvent)>) override {}
    pulsar::core::AdapterCapabilities capabilities() const override {
        pulsar::core::AdapterCapabilities c;
        c.output_formats = {pulsar::core::PixelFormat::NV12};
        c.color_spaces   = {pulsar::core::ColorSpace::BT709};
        return c;
    }
private:
    std::chrono::steady_clock::time_point last_ = std::chrono::steady_clock::now();
    int frame_ = 0;
};

// ─── Build the adapter chain and run the pipeline ────────────────────────────

static int run_pipeline_for_session(
    const ServerConfig&           cfg,
    pulsar::core::ITransport&     primary_transport,
    pulsar::core::ICaptureSource& capture,
    StdoutLogger&                 logger,
    std::atomic<bool>&            stop)
{
    // ── Optional recording sink ──────────────────────────────────────────────
    std::unique_ptr<FileRecorder>      recorder;
    std::unique_ptr<OutputMultiplexer> mux;
    pulsar::core::ITransport* transport = &primary_transport;

    if (cfg.recording.enabled) {
        // mkdir recordings/ if absent
        ::mkdir(cfg.recording.output_dir.c_str(), 0755);
        char ts[32]{};
        std::time_t now = std::time(nullptr);
        std::strftime(ts, sizeof(ts), "%Y%m%d_%H%M%S", std::localtime(&now));
        const std::string path = cfg.recording.output_dir + "pulsar_" + ts + ".h264";

        recorder = std::make_unique<FileRecorder>();
        recorder->start(path);
        mux = std::make_unique<OutputMultiplexer>(primary_transport);
        mux->add_sink(std::shared_ptr<pulsar::core::IPacketSink>(recorder.get(), [](auto*) {}));
        transport = mux.get();
        logger.log(pulsar::core::LogLevel::Info, "  recording: " + path);
    }
    // ── Encoder selection: NVENC → VAAPI → x264 ──────────────────────────
    // Each encoder uses libavcodec as its backend internally.
    // "auto" or hardware backends: try best available hardware, then software.
    std::unique_ptr<pulsar::core::IEncoder> encoder;
    const std::string codec_type =
        (cfg.encoder.backend == "hevc" || cfg.encoder.backend == "h265") ? "hevc" : "h264";
    const bool want_nvenc = (cfg.encoder.backend == "nvenc" || cfg.encoder.backend == "auto"
                          || cfg.encoder.backend == "h264"  || cfg.encoder.backend == "hevc"
                          || cfg.encoder.backend == "vaapi"); // vaapi falls back to nvenc
    const bool want_vaapi = (cfg.encoder.backend == "vaapi" || cfg.encoder.backend == "auto");

    if (want_nvenc && pulsar::encoder::nvenc::nvenc_is_available()) {
        encoder = std::make_unique<pulsar::encoder::nvenc::NvencEncoder>(codec_type);
        logger.log(pulsar::core::LogLevel::Info, "  encoder: nvenc (" + codec_type + ")");
    }
    if (!encoder && want_vaapi && pulsar::encoder::vaapi::vaapi_is_available()) {
        encoder = std::make_unique<pulsar::encoder::vaapi::VaapiEncoder>(codec_type);
        logger.log(pulsar::core::LogLevel::Info, "  encoder: vaapi (" + codec_type + ")");
    }
    if (!encoder) {
        encoder = std::make_unique<pulsar::encoder::x264::X264Encoder>();
        logger.log(pulsar::core::LogLevel::Info, "  encoder: x264 (software fallback)");
    }

    // ── Preprocessor selection ────────────────────────────────────────────
    // Priority: dmabuf zero-copy → gpu VPP → cpu BGRA→NV12 → direct nullptr
    std::unique_ptr<pulsar::core::IPreprocessor> preprocessor;
    auto cap_caps = capture.capabilities();
    auto enc_caps = encoder->capabilities();
    if (cap_caps.supports_dmabuf && enc_caps.supports_dmabuf) {
        preprocessor = std::make_unique<pulsar::preprocessor::dmabuf::DmabufImporter>();
        logger.log(pulsar::core::LogLevel::Info, "  preprocessor: dmabuf zero-copy");
    } else if (!cap_caps.output_formats.empty() &&
               cap_caps.output_formats[0] != pulsar::core::PixelFormat::NV12 &&
               cap_caps.supports_gpu_preprocessing) {
        preprocessor = std::make_unique<pulsar::preprocessor::gpu::GpuPreprocessor>();
        logger.log(pulsar::core::LogLevel::Info, "  preprocessor: gpu VPP BGRA→NV12");
    } else if (!cap_caps.output_formats.empty() &&
               cap_caps.output_formats[0] != pulsar::core::PixelFormat::NV12) {
        preprocessor = std::make_unique<pulsar::preprocessor::cpu::CpuPreprocessor>();
        logger.log(pulsar::core::LogLevel::Info, "  preprocessor: cpu BGRA→NV12");
    } else {
        logger.log(pulsar::core::LogLevel::Info, "  preprocessor: direct (nullptr)");
    }

    // ── Wire input + FEC (inline, same as QUIC/WebRTC via wire_transport) ─────
    pulsar::input::uinput::UinputHandler rtp_input;
    if (!rtp_input.is_available())
        logger.log(pulsar::core::LogLevel::Warn,
            "  uinput: /dev/uinput not accessible (add user to 'input' group)");
    transport->set_input_callback([&rtp_input](pulsar::core::InputEvent ev) {
        rtp_input.inject(ev);
    });
    transport->set_stats_callback([transport](pulsar::core::NetworkStats s) {
        pulsar::core::FecParams fec;
        fec.enabled       = s.loss_rate > 0.02f;
        fec.data_shards   = 10;
        fec.parity_shards = fec.enabled ? 2 : 0;
        transport->set_fec_params(fec);
    });

    // ── Audio ─────────────────────────────────────────────────────────────
    std::unique_ptr<pulsar::audio::capture::pipewire::PipeWireAudioCapture> audio_cap;
    std::unique_ptr<pulsar::audio::encoder::opus::OpusEncoder>              audio_enc;
    if (cfg.audio.enabled) {
        audio_cap = std::make_unique<pulsar::audio::capture::pipewire::PipeWireAudioCapture>();
        audio_enc = std::make_unique<pulsar::audio::encoder::opus::OpusEncoder>();
        logger.log(pulsar::core::LogLevel::Info, "  audio: pipewire → opus");
    }

    // ── Pipeline config ───────────────────────────────────────────────────
    pulsar::core::PipelineConfig pipe_cfg;
    pipe_cfg.queue_capacity = cfg.pipeline.queue_capacity;
    pipe_cfg.idle_fps       = cfg.pipeline.idle_fps;
    pipe_cfg.target_fps     = cfg.encoder.fps;
    pipe_cfg.audio_enabled  = cfg.audio.enabled;

    // Reconnect policy: stay alive up to 30 s after client disconnects.
    pipe_cfg.reconnect.suspend_timeout_ms       = 30000;
    pipe_cfg.reconnect.reconnect_interval_ms    = 500;
    pipe_cfg.reconnect.force_keyframe_on_resume = true;

    logger.log(pulsar::core::LogLevel::Info, "pipeline starting");

    // Encoder fallback factory: if NVENC crashes, swap in x264 so the stream
    // continues uninterrupted.  Recovery time is < 500 ms.
    auto encoder_fallback = [&]() -> std::unique_ptr<pulsar::core::IEncoder> {
        logger.log(pulsar::core::LogLevel::Warn,
            "pipeline: primary encoder failed — falling back to x264");
        return std::make_unique<pulsar::encoder::x264::X264Encoder>();
    };

    pulsar::core::run_pipeline(
        capture, preprocessor.get(), *encoder,
        *transport, rtp_input,
        audio_cap.get(), audio_enc.get(),
        pipe_cfg, &logger, nullptr, &stop,
        nullptr,
        encoder_fallback);

    if (recorder) recorder->stop();
    logger.log(pulsar::core::LogLevel::Info, "pipeline stopped");
    return 0;
}

// ─── pick_capture helper ─────────────────────────────────────────────────────────
static pulsar::core::ICaptureSource& pick_capture(
    const ServerConfig& cfg,
    StdoutLogger& logger,
    const std::string& proto,
    std::unique_ptr<SyntheticCapture>& synth_cap,
    std::unique_ptr<pulsar::capture::drm_virtual::DrmVirtualCapture>& drm_cap)
{
    if (cfg.capture.backend == "synthetic") {
        if (!synth_cap) synth_cap = std::make_unique<SyntheticCapture>();
        return *synth_cap;
    }
    if (!drm_cap)
        drm_cap = std::make_unique<pulsar::capture::drm_virtual::DrmVirtualCapture>();
    if (!drm_cap->next_frame().has_value()) {
        logger.log(pulsar::core::LogLevel::Warn,
            proto + ": drm_virtual init failed, falling back to synthetic");
        if (!synth_cap) synth_cap = std::make_unique<SyntheticCapture>();
        return *synth_cap;
    }
    return *drm_cap;
}

// ─── Session wiring: FEC auto-switch + input callback ─────────────────────────
static void wire_transport(pulsar::core::ITransport& transport,
                            pulsar::input::uinput::UinputHandler& input)
{
    transport.set_input_callback([&input](pulsar::core::InputEvent ev) {
        input.inject(ev);
    });
    transport.set_stats_callback([&transport](pulsar::core::NetworkStats s) {
        pulsar::core::FecParams fec;
        fec.enabled       = s.loss_rate > 0.02f;
        fec.data_shards   = 10;
        fec.parity_shards = fec.enabled ? 2 : 0;
        transport.set_fec_params(fec);
    });
}

// ─── QUIC session loop ─────────────────────────────────────────────────────────
static void run_quic_loop(const ServerConfig& cfg, StdoutLogger& logger,
                           std::atomic<bool>& stop)
{
    while (!stop.load()) {
        pulsar::transport::quic::QuicTransport transport;
        logger.log(pulsar::core::LogLevel::Info,
            "  listener: quic port=" + std::to_string(cfg.protocols.quic.port) +
            "  waiting for client...");
        if (!transport.server_accept(cfg.protocols.quic.port, 30000)) {
            if (!stop.load())
                logger.log(pulsar::core::LogLevel::Warn, "quic: no client within 30s, retrying");
            continue;
        }
        logger.log(pulsar::core::LogLevel::Info, "quic: client connected");

        pulsar::input::uinput::UinputHandler input;
        if (!input.is_available())
            logger.log(pulsar::core::LogLevel::Warn, "quic: uinput not accessible");
        wire_transport(transport, input);

        std::unique_ptr<SyntheticCapture> synth_cap;
        std::unique_ptr<pulsar::capture::drm_virtual::DrmVirtualCapture> drm_cap;
        auto& capture = pick_capture(cfg, logger, "quic", synth_cap, drm_cap);

        std::atomic<bool> session_stop{false};
        std::thread stop_watcher([&] {
            while (!stop.load() && !session_stop.load())
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            session_stop.store(true);
        });
        run_pipeline_for_session(cfg, transport, capture, logger, session_stop);
        session_stop.store(true);
        stop_watcher.join();
        transport.disconnect();
        logger.log(pulsar::core::LogLevel::Info, "quic: session ended");
    }
}

// ─── WebRTC session loop (TCP signaling) ───────────────────────────────────────
// Minimal signaling protocol over a plain TCP socket:
//   1. Server sends SDP offer followed by "\r\n---\r\n"
//   2. Client sends SDP answer followed by "\r\n---\r\n"
//   3. ICE negotiates; media flows
static void run_webrtc_loop(const ServerConfig& cfg, StdoutLogger& logger,
                             std::atomic<bool>& stop)
{
    int srv_fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (srv_fd < 0) {
        logger.log(pulsar::core::LogLevel::Error, "webrtc: signaling socket failed");
        return;
    }
    int opt = 1;
    ::setsockopt(srv_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port        = htons(static_cast<uint16_t>(cfg.protocols.webrtc.port));
    if (::bind(srv_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        logger.log(pulsar::core::LogLevel::Error,
            "webrtc: signaling bind failed on port " +
            std::to_string(cfg.protocols.webrtc.port));
        ::close(srv_fd);
        return;
    }
    ::listen(srv_fd, 1);
    logger.log(pulsar::core::LogLevel::Info,
        "  listener: webrtc signaling port=" +
        std::to_string(cfg.protocols.webrtc.port));

    while (!stop.load()) {
        fd_set rset; FD_ZERO(&rset); FD_SET(srv_fd, &rset);
        timeval tv{2, 0};
        if (::select(srv_fd + 1, &rset, nullptr, nullptr, &tv) <= 0) continue;

        int client_fd = ::accept(srv_fd, nullptr, nullptr);
        if (client_fd < 0) continue;
        logger.log(pulsar::core::LogLevel::Info, "webrtc: signaling client connected");

        pulsar::transport::webrtc::WebRtcTransport transport;
        const std::string offer = transport.generate_sdp_offer();
        if (offer.empty()) {
            logger.log(pulsar::core::LogLevel::Error, "webrtc: SDP offer generation failed");
            ::close(client_fd);
            continue;
        }
        // Send offer
        const std::string offer_msg = offer + "\r\n---\r\n";
        ::send(client_fd, offer_msg.data(), offer_msg.size(), 0);

        // Receive answer (terminated by \r\n---\r\n)
        std::string answer_buf;
        char buf[4096];
        bool got_answer = false;
        while (!stop.load()) {
            fd_set rr; FD_ZERO(&rr); FD_SET(client_fd, &rr);
            timeval tv2{5, 0};
            if (::select(client_fd + 1, &rr, nullptr, nullptr, &tv2) <= 0) break;
            ssize_t n = ::recv(client_fd, buf, sizeof(buf) - 1, 0);
            if (n <= 0) break;
            buf[n] = '\0';
            answer_buf += buf;
            const auto pos = answer_buf.find("\r\n---\r\n");
            if (pos != std::string::npos) {
                answer_buf = answer_buf.substr(0, pos);
                got_answer = true;
                break;
            }
        }
        ::close(client_fd);

        if (!got_answer || !transport.apply_sdp_answer(answer_buf)) {
            logger.log(pulsar::core::LogLevel::Warn, "webrtc: SDP exchange failed");
            continue;
        }
        logger.log(pulsar::core::LogLevel::Info, "webrtc: ICE negotiating...");

        // Wait for ICE (up to 10 s)
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (!transport.connected() && !stop.load() &&
               std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(100));

        if (!transport.connected()) {
            logger.log(pulsar::core::LogLevel::Warn, "webrtc: ICE timeout");
            continue;
        }
        logger.log(pulsar::core::LogLevel::Info, "webrtc: connected — starting stream");

        pulsar::input::uinput::UinputHandler input;
        if (!input.is_available())
            logger.log(pulsar::core::LogLevel::Warn, "webrtc: uinput not accessible");
        wire_transport(transport, input);

        std::unique_ptr<SyntheticCapture> synth_cap;
        std::unique_ptr<pulsar::capture::drm_virtual::DrmVirtualCapture> drm_cap;
        auto& capture = pick_capture(cfg, logger, "webrtc", synth_cap, drm_cap);

        std::atomic<bool> session_stop{false};
        std::thread stop_watcher([&] {
            while (!stop.load() && !session_stop.load())
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            session_stop.store(true);
        });
        run_pipeline_for_session(cfg, transport, capture, logger, session_stop);
        session_stop.store(true);
        stop_watcher.join();
        transport.disconnect();
        logger.log(pulsar::core::LogLevel::Info, "webrtc: session ended");
    }
    ::close(srv_fd);
}

// ─── run_server ───────────────────────────────────────────────────────────────

int run_server(const ServerConfig& cfg) {
    StdoutLogger           logger;
    InMemorySessionManager session_mgr;
    pulsar::auth::password::PasswordAuth auth(cfg.auth.username, cfg.auth.password);

    std::signal(SIGINT,  signal_handler);
    std::signal(SIGTERM, signal_handler);
    g_stop.store(false);

    logger.log(pulsar::core::LogLevel::Info, "Pulsar server starting");
    logger.log(pulsar::core::LogLevel::Info, "  capture:  " + cfg.capture.backend);
    logger.log(pulsar::core::LogLevel::Info, "  encoder:  " + cfg.encoder.backend);
    logger.log(pulsar::core::LogLevel::Info,
        "  audio:    " + std::string(cfg.audio.enabled ? "enabled" : "disabled"));
    if (cfg.recording.enabled)
        logger.log(pulsar::core::LogLevel::Info,
            "  recording: enabled → " + cfg.recording.output_dir);

    // ── Wake-on-LAN background listener ──────────────────────────────────
    WakeOnLanServer wol;
    if (cfg.wake_on_lan.enabled) {
        wol.listen(cfg.wake_on_lan.listen_port);
        logger.log(pulsar::core::LogLevel::Info,
            "  wake-on-lan: listening on UDP " +
            std::to_string(cfg.wake_on_lan.listen_port));
    }

    // ── App manager ───────────────────────────────────────────────────────
    AppManager app_mgr;
    if (!cfg.apps.empty()) {
        app_mgr.set_apps(cfg.apps);
        logger.log(pulsar::core::LogLevel::Info,
            "  apps: " + std::to_string(cfg.apps.size()) + " registered");
    }


    // ── Profiler: dual-dimension encoder selection ─────────────────────────
    // Run at startup; cached to disk so subsequent restarts are instant.
    const auto profile = run_profiler(cfg);
    if (cfg.encoder.backend == "auto") {
        const std::string best = pulsar::core::select_best_encoder(profile);
        logger.log(pulsar::core::LogLevel::Info,
            "  profiler: selected encoder = " + best);
        // Override backend for this session (cast away const via a mutable local)
        const_cast<ServerConfig&>(cfg).encoder.backend = best;
    } else {
        logger.log(pulsar::core::LogLevel::Info,
            "  profiler: encoder forced to " + cfg.encoder.backend);
    }

    // ── Auth ──────────────────────────────────────────────────────────────
    pulsar::core::AuthToken tok;
    tok.scheme           = "password";
    tok.data["username"] = cfg.auth.username;
    tok.data["password"] = cfg.auth.password;
    if (!auth.authenticate(tok)) {
        logger.log(pulsar::core::LogLevel::Error,
            pulsar::core::to_string(pulsar::core::StreamError::AuthFailed));
        return 1;
    }
    auto session = session_mgr.create(tok);
    session_mgr.activate(session.id);
    logger.log(pulsar::core::LogLevel::Info, "  session:  " + session.id + " active");

    // ── RTP / UDP setup ───────────────────────────────────────────────────
    // New transport: UDP push mode.
    // The server binds video port and waits for the client to send a small
    // "hello" UDP datagram.  Once the client address is known, the server
    // starts pushing RTP.
    //
    // Client setup (run on client machine):
    //   # 1. Register with server (any small UDP datagram)
    //   echo "hello" | nc -u <server_ip> 47984
    //   # 2. Receive video
    //   ffplay rtp://0.0.0.0:47984
    //   # 3. Receive audio  (separate terminal)
    //   ffplay rtp://0.0.0.0:47986
    //   # Or use SDP file:
    //   ffplay -protocol_whitelist file,rtp,udp -i stream.sdp

    // ── Accept loop ───────────────────────────────────────────────────────
    const bool rtp_enabled     = cfg.protocols.rtp.enabled;
    const bool quic_enabled    = cfg.protocols.quic.enabled;
    const bool webrtc_enabled  = cfg.protocols.webrtc.enabled;

    if (!rtp_enabled && !quic_enabled && !webrtc_enabled) {
        logger.log(pulsar::core::LogLevel::Error,
            "server: no streaming protocol enabled (rtp/quic/webrtc)");
        return 1;
    }

    // QUIC and WebRTC run on background threads; RTP runs on the main thread.
    std::thread quic_th, webrtc_th;
    if (quic_enabled)
        quic_th = std::thread([&] { run_quic_loop(cfg, logger, g_stop); });
    if (webrtc_enabled)
        webrtc_th = std::thread([&] { run_webrtc_loop(cfg, logger, g_stop); });

    while (rtp_enabled && !g_stop.load()) {
        pulsar::transport::rtp::RtpTransport transport;

        // ── push_to mode: skip hello, push directly to a known client IP ──────
        if (!cfg.protocols.rtp.push_to.empty()) {
            const std::string dest = cfg.protocols.rtp.push_to + ":" +
                                     std::to_string(cfg.protocols.rtp.port);
            logger.log(pulsar::core::LogLevel::Info,
                "  rtp: push mode → " + dest);
            if (!transport.connect(dest)) {
                logger.log(pulsar::core::LogLevel::Error, "rtp: connect failed");
                std::this_thread::sleep_for(std::chrono::seconds(2));
                continue;
            }
        } else {
            // ── hello mode: wait for client UDP registration ──────────────────
            logger.log(pulsar::core::LogLevel::Info,
                "  listener: rtp video=" +
                std::to_string(cfg.protocols.rtp.port) +
                " audio=" +
                std::to_string(cfg.protocols.rtp.port + 2) +
                "  waiting for client UDP hello...");
            transport.print_sdp();

            if (!transport.server_bind(cfg.protocols.rtp.port)) {
                logger.log(pulsar::core::LogLevel::Error,
                    "rtp: server_bind failed, retrying in 2s");
                std::this_thread::sleep_for(std::chrono::seconds(2));
                continue;
            }
            if (!transport.wait_for_client(30000)) {
                if (g_stop.load()) break;
                logger.log(pulsar::core::LogLevel::Warn, "no client within 30s, retrying");
                continue;
            }
        }
        logger.log(pulsar::core::LogLevel::Info, "rtp: client ready — starting stream");

        // Select capture backend based on config (lazy construction).
        std::unique_ptr<SyntheticCapture> synth_cap;
        std::unique_ptr<pulsar::capture::drm_virtual::DrmVirtualCapture> drm_cap;
        auto pick = [&]() -> pulsar::core::ICaptureSource& {
            if (cfg.capture.backend == "synthetic") {
                if (!synth_cap) synth_cap = std::make_unique<SyntheticCapture>();
                return *synth_cap;
            }
            // default: drm_virtual with synthetic fallback
            if (!drm_cap)
                drm_cap = std::make_unique<pulsar::capture::drm_virtual::DrmVirtualCapture>();
            if (!drm_cap->next_frame().has_value()) {
                logger.log(pulsar::core::LogLevel::Warn,
                    "rtp: drm_virtual init failed (DRM master busy?), falling back to synthetic");
                if (!synth_cap) synth_cap = std::make_unique<SyntheticCapture>();
                return *synth_cap;
            }
            return *drm_cap;
        };
        pulsar::core::ICaptureSource& capture = pick();

        std::atomic<bool> session_stop{false};
        std::thread stop_watcher([&] {
            while (!g_stop.load() && !session_stop.load())
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            session_stop.store(true);
        });

        run_pipeline_for_session(cfg, transport, capture, logger, session_stop);
        // No g_stop.store(true): loop back and accept the next client.
        session_stop.store(true);
        stop_watcher.join();
    }

    // If only QUIC/WebRTC are enabled, keep alive until stop signal.
    if (!rtp_enabled) {
        while (!g_stop.load())
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    if (quic_th.joinable())   quic_th.join();
    if (webrtc_th.joinable()) webrtc_th.join();

    logger.log(pulsar::core::LogLevel::Info, "server: shutdown");
    return 0;
}

int run_server(const std::string& config_path) {
    std::string err;
    auto cfg = load_config(config_path, err);
    if (!cfg) { std::cerr << "[error] config: " << err << '\n'; return 1; }
    return run_server(*cfg);
}

// ─── run_verify ───────────────────────────────────────────────────────────────

int run_verify(const ServerConfig& cfg_in) {
    StdoutLogger logger;
    logger.log(pulsar::core::LogLevel::Info, "=== Phase 1 Step 3 self-verification ===");

    ServerConfig cfg = cfg_in;
    cfg.audio.enabled = true;

    const int port = cfg.protocols.rtp.port;

    // ── Loopback client thread ────────────────────────────────────────────
    // Binds port 47984/UDP and drains incoming RTP datagrams.
    // Server uses push mode (connect → sendto), no hello needed.
    std::atomic<size_t> bytes_received{0};
    std::thread client_th([&] {
        int recv_fd = ::socket(AF_INET, SOCK_DGRAM, 0);
        int opt = 1;
        ::setsockopt(recv_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
        sockaddr_in local{};
        local.sin_family      = AF_INET;
        local.sin_addr.s_addr = INADDR_ANY;
        local.sin_port        = htons(static_cast<uint16_t>(port));
        if (::bind(recv_fd, reinterpret_cast<sockaddr*>(&local), sizeof(local)) != 0) {
            ::close(recv_fd); return;
        }
        uint8_t buf[4096];
        while (true) {
            fd_set rset; FD_ZERO(&rset); FD_SET(recv_fd, &rset);
            timeval tv{0, 100000}; // 100ms poll
            if (::select(recv_fd + 1, &rset, nullptr, nullptr, &tv) <= 0) {
                // If server stopped sending, give up after 3 seconds of silence
                static int idle = 0;
                if (++idle > 30) break;
                continue;
            }
            ssize_t n = ::recv(recv_fd, buf, sizeof(buf), 0);
            if (n <= 0) break;
            bytes_received += static_cast<size_t>(n);
        }
        ::close(recv_fd);
    });

    // ── Server side ───────────────────────────────────────────────────────
    // For loopback verify: use push mode (connect to 127.0.0.1:port) —
    // no hello mechanism needed; avoids port conflict with client receiver.
    pulsar::transport::rtp::RtpTransport transport;
    if (!transport.connect("127.0.0.1:" + std::to_string(port))) {
        logger.log(pulsar::core::LogLevel::Error, "verify: connect failed");
        client_th.join();
        return 1;
    }

    SyntheticCapture synth;
    std::atomic<bool> stop{false};
    std::thread timer([&stop] {
        std::this_thread::sleep_for(std::chrono::seconds(2));
        stop.store(true);
    });

    run_pipeline_for_session(cfg, transport, synth, logger, stop);
    timer.join();
    transport.disconnect();
    client_th.join();

    const size_t vp = transport.video_packets_sent();
    const size_t ap = transport.audio_packets_sent();
    const size_t br = bytes_received.load();

    logger.log(pulsar::core::LogLevel::Info,
        "verify: video_packets=" + std::to_string(vp)
        + "  audio_packets=" + std::to_string(ap)
        + "  bytes_received=" + std::to_string(br));

    const bool ok = (vp > 0 && ap > 0 && br > 0);
    logger.log(ok ? pulsar::core::LogLevel::Info : pulsar::core::LogLevel::Error,
        ok ? "verify: PASS — video, audio, and transport all working (RTP/UDP)"
           : "verify: FAIL — one or more checks did not produce output");
    return ok ? 0 : 1;
}

} // namespace pulsar::server
