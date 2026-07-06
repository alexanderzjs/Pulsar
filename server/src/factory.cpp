#include "factory.h"
#include "config_parser.h"
#include "server_profiler.h"
#include "session_services.h"
#include "session_detect.h"

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
#include "xcb_capture.h"
#include "pipewire_capture.h"

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
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstring>
#include <ctime>
#include <iomanip>
#include <iostream>
#include <linux/input-event-codes.h>
#include <memory>
#include <netinet/in.h>
#include <nlohmann/json.hpp>
#include <optional>
#include <sstream>
#include <string>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <unordered_map>
#include <vector>

namespace pulsar::server {

static std::string log_stamp() {
    using namespace std::chrono;
    const auto now = system_clock::now();
    const auto tt = system_clock::to_time_t(now);
    const auto ms = duration_cast<milliseconds>(now.time_since_epoch()) % 1000;
    std::tm tm{};
#if defined(_WIN32)
    localtime_s(&tm, &tt);
#else
    localtime_r(&tt, &tm);
#endif
    std::ostringstream oss;
    oss << '[' << std::put_time(&tm, "%H:%M:%S") << '.'
        << std::setw(3) << std::setfill('0') << ms.count() << "] ";
    return oss.str();
}

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
        std::cout << log_stamp() << '[' << lv << "] " << msg << '\n';
        std::cout.flush();
    }
};

// ─── InMemorySessionManager ───────────────────────────────────────────────────
class InMemorySessionManager final : public pulsar::core::ISessionManager {
public:
    pulsar::core::Session create(const pulsar::core::AuthToken& tok) override {
        std::lock_guard lk(mu_);
        pulsar::core::Session s;
        s.id    = tok.data.count("username") ? tok.data.at("username") : "session-1";
        s.state = pulsar::core::SessionState::Authenticating;
        sessions_[s.id] = s;
        return s;
    }
    void terminate(const std::string& id) override {
        std::lock_guard lk(mu_);
        sessions_.erase(id);
    }
    pulsar::core::Session* find(const std::string& id) override {
        std::lock_guard lk(mu_);
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
    mutable std::mutex mu_;
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
    std::atomic<bool>&            stop,
    pulsar::core::IInputHandler&  input,
    bool                          persistent = false)
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
    // ── Encoder + Audio: parallel init to minimize startup latency ───────────
    const std::string codec_type =
        (cfg.encoder.backend == "hevc" || cfg.encoder.backend == "h265") ? "hevc" : "h264";
    const bool want_nvenc = (cfg.encoder.backend == "nvenc" || cfg.encoder.backend == "auto"
                          || cfg.encoder.backend == "h264"  || cfg.encoder.backend == "hevc"
                          || cfg.encoder.backend == "vaapi");
    const bool want_vaapi = (cfg.encoder.backend == "vaapi" || cfg.encoder.backend == "auto");
    std::unique_ptr<pulsar::core::IEncoder> encoder;
    std::thread enc_init_th([&] {
        if (want_nvenc && pulsar::encoder::nvenc::nvenc_is_available()) {
            encoder = std::make_unique<pulsar::encoder::nvenc::NvencEncoder>(codec_type);
            logger.log(pulsar::core::LogLevel::Info, "  encoder: nvenc (" + codec_type + ")");
        } else if (want_vaapi && pulsar::encoder::vaapi::vaapi_is_available()) {
            encoder = std::make_unique<pulsar::encoder::vaapi::VaapiEncoder>(codec_type);
            logger.log(pulsar::core::LogLevel::Info, "  encoder: vaapi (" + codec_type + ")");
        } else {
            encoder = std::make_unique<pulsar::encoder::x264::X264Encoder>();
            logger.log(pulsar::core::LogLevel::Info, "  encoder: x264 (software fallback)");
        }
    });

    enc_init_th.join();  // must complete before using encoder->capabilities()

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
    transport->set_input_callback([&input](pulsar::core::InputEvent ev) {
        input.inject(ev);
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

    // Reconnect policy: for persistent pipeline, never timeout.
    // For per-session pipelines, stay alive up to 30s after client disconnects.
    pipe_cfg.reconnect.suspend_timeout_ms       = persistent ? -1 : 30000;
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
        *transport, input,
        audio_cap.get(), audio_enc.get(),
        pipe_cfg, &logger, nullptr, &stop,
        nullptr,
        encoder_fallback);

    if (recorder) recorder->stop();
    logger.log(pulsar::core::LogLevel::Info, "pipeline stopped");
    return 0;
}

// ─── pick_capture helper ─────────────────────────────────────────────────────────
// shared_pw_cap: server-lifetime PipeWire capture (never destroyed between sessions).
// Per-session pw_cap is still accepted as fallback but will be unused for pipewire.
static pulsar::core::ICaptureSource& pick_capture(
    const ServerConfig& cfg,
    StdoutLogger& logger,
    const std::string& proto,
    std::unique_ptr<SyntheticCapture>& synth_cap,
    std::unique_ptr<pulsar::capture::drm_virtual::DrmVirtualCapture>& drm_cap,
    std::unique_ptr<pulsar::capture::xcb::XcbCapture>& xcb_cap,
    std::unique_ptr<pulsar::capture::pipewire::PipeWireCapture>& /*pw_cap_unused*/,
    std::shared_ptr<pulsar::capture::pipewire::PipeWireCapture> shared_pw_cap = nullptr)
{
    if (cfg.capture.backend == "synthetic") {
        if (!synth_cap) synth_cap = std::make_unique<SyntheticCapture>();
        return *synth_cap;
    }
    if (cfg.capture.backend == "pipewire") {
        // Use the server-level singleton: same virtual monitor across all sessions
        if (shared_pw_cap && shared_pw_cap->is_open()) return *shared_pw_cap;
        logger.log(pulsar::core::LogLevel::Warn,
            proto + ": pipewire capture not available, falling back to synthetic");
        if (!synth_cap) synth_cap = std::make_unique<SyntheticCapture>();
        return *synth_cap;
    }
    if (cfg.capture.backend == "xcb") {
        if (!xcb_cap) {
            if (!cfg.capture.xcb_xauthority.empty())
                ::setenv("XAUTHORITY", cfg.capture.xcb_xauthority.c_str(), 1);
            xcb_cap = std::make_unique<pulsar::capture::xcb::XcbCapture>(
                cfg.capture.xcb_display);
        }
        if (!xcb_cap->is_open()) {
            logger.log(pulsar::core::LogLevel::Warn,
                proto + ": xcb cannot connect to " + cfg.capture.xcb_display +
                ", falling back to synthetic");
            if (!synth_cap) synth_cap = std::make_unique<SyntheticCapture>();
            return *synth_cap;
        }
        return *xcb_cap;
    }
    // default: drm_virtual with synthetic fallback
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
                            pulsar::core::IInputHandler& input)
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
                           std::atomic<bool>& stop,
                           pulsar::core::IInputHandler& input,
                           std::shared_ptr<pulsar::capture::pipewire::PipeWireCapture> shared_pw_cap = nullptr)
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

        wire_transport(transport, input);

        std::unique_ptr<SyntheticCapture> synth_cap;
        std::unique_ptr<pulsar::capture::drm_virtual::DrmVirtualCapture> drm_cap;
        std::unique_ptr<pulsar::capture::xcb::XcbCapture> xcb_cap;
        std::unique_ptr<pulsar::capture::pipewire::PipeWireCapture> pw_cap;
        auto& capture = pick_capture(cfg, logger, "quic", synth_cap, drm_cap, xcb_cap, pw_cap, shared_pw_cap);

        std::atomic<bool> session_stop{false};
        std::thread stop_watcher([&] {
            while (!stop.load() && !session_stop.load())
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            session_stop.store(true);
        });
        run_pipeline_for_session(cfg, transport, capture, logger, session_stop, input);
        session_stop.store(true);
        stop_watcher.join();
        transport.disconnect();
        logger.log(pulsar::core::LogLevel::Info, "quic: session ended");
    }
}

// ─── SseTransport: HTTP video stream via Server-Sent Events ──────────────────
// Sends H264 NALUs as SSE events (base64). Works through SSH tunnel (pure TCP).
// Browser uses WebCodecs VideoDecoder to decode and render.
class SseTransport final : public pulsar::core::ITransport {
    int  fd_;
    bool closed_ = false;
    std::mutex mu_;       // protects closed_ and heartbeat sends
    std::mutex send_mu_;  // serializes all socket writes (video + heartbeat)

    static std::string b64(const uint8_t* data, size_t len) {
        static const char T[] =
            "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        std::string o; o.reserve((len+2)/3*4);
        for (size_t i = 0; i < len; i += 3) {
            uint32_t n = (uint32_t)data[i]<<16;
            if (i+1<len) n|=(uint32_t)data[i+1]<<8;
            if (i+2<len) n|=(uint32_t)data[i+2];
            o+=T[(n>>18)&63]; o+=T[(n>>12)&63];
            o+=(i+1<len)?T[(n>>6)&63]:'=';
            o+=(i+2<len)?T[(n)&63]:'=';
        }
        return o;
    }

    void sse(const char* ev, const uint8_t* data, size_t len) {
        std::string msg = std::string("event:")+ev+"\ndata:"+b64(data,len)+"\n\n";
        // serialize all socket writes to prevent interleaving (pipeline send
        // thread vs heartbeat thread both write to the same fd).
        std::lock_guard send_lk(send_mu_);
        {
            std::lock_guard lk(mu_);
            if (closed_) return;
        }
        const char* ptr = msg.data();
        size_t remaining = msg.size();
        while (remaining > 0) {
            ssize_t sent = ::send(fd_, ptr, remaining, MSG_NOSIGNAL);
            if (sent < 0) {
                if (errno == EINTR) continue;
                std::lock_guard lk(mu_); closed_ = true;
                return;
            }
            ptr      += sent;
            remaining -= static_cast<size_t>(sent);
        }
    }

public:
    explicit SseTransport(int fd) : fd_(fd) {}
    ~SseTransport() { ::close(fd_); }
    bool is_alive() const { return !closed_; }

    // Sends event:stop to tell the browser to call es.close() (no auto-reconnect).
    // Called when this session is preempted by a newer one.
    void preempt() {
        std::lock_guard lk(mu_);
        if (!closed_) {
            const char evt[] = "event:stop\ndata:preempted\n\n";
            ::send(fd_, evt, sizeof(evt)-1, MSG_NOSIGNAL | MSG_DONTWAIT);
        }
    }

    // Parse Annex-B stream: classify event type, send whole packet as-is
    void send(pulsar::core::EncodedPacket pkt) override {
        const uint8_t* d = pkt.buffer ? pkt.buffer->data() : nullptr;
        size_t sz = pkt.buffer ? pkt.buffer->size() : 0;
        if (!pkt.buffer || closed_) return;

        // Scan for SPS (type 7) and IDR (type 5)
        bool has_sps = false, has_idr = false;
        for (size_t i = 0; i + 4 < sz; ++i) {
            if (d[i]==0&&d[i+1]==0&&d[i+2]==0&&d[i+3]==1) {
                uint8_t nt = d[i+4] & 0x1F;
                if (nt == 7) has_sps = true;
                if (nt == 5) has_idr = true;
            }
        }
        // If packet contains IDR, it's a keyframe (may also carry SPS/PPS)
        // sps_pps-only packets (no IDR) are sent separately for early configure
        const char* ev = has_idr                  ? "key"
                       : has_sps                  ? "sps_pps"
                       : pkt.is_keyframe          ? "key"
                       :                            "delta";
        static thread_local int logged = 0;
        if (logged < 12 || has_idr || has_sps || pkt.is_keyframe) {
            std::cerr << log_stamp() << "[sse] send event=" << ev
                      << " size=" << sz
                      << " key=" << pkt.is_keyframe
                      << " idr=" << has_idr
                      << " sps=" << has_sps
                      << "\n";
            ++logged;
        }
        sse(ev, d, sz);
    }
    void send_batch(std::vector<pulsar::core::EncodedPacket> v) override {
        for (auto& p : v) send(std::move(p));
    }
    void send_audio(pulsar::core::AudioPacket) override {}
    void heartbeat() {
        // SSE comment line — used to detect dead connections
        const char ping[] = ": h\n\n";
        std::lock_guard send_lk(send_mu_);  // serialize with video sends
        std::lock_guard lk(mu_);
        if (!closed_ && ::send(fd_, ping, sizeof(ping)-1, MSG_NOSIGNAL) < 0)
            closed_ = true;
    }

    bool connect(const std::string&) override { return true; }
    void disconnect() override { std::lock_guard lk(mu_); closed_=true; }
    void set_event_callback(std::function<void(pulsar::core::TransportEvent)> cb) override {
        if (cb) cb(pulsar::core::TransportEvent::Ready);
    }
    void set_stats_callback(std::function<void(pulsar::core::NetworkStats)>) override {}
    void set_input_callback(std::function<void(pulsar::core::InputEvent)>) override {}
    void set_mic_callback(std::function<void(pulsar::core::AudioFrame)>) override {}
    void set_jitter_buffer(int,int) override {}
    void set_fec_params(const pulsar::core::FecParams&) override {}
    void send_haptic(const pulsar::core::HapticCommand&) override {}
    void send_stats(const pulsar::core::PipelineMetrics&) override {}
    std::string sink_id() const override { return "sse"; }
    void on_packet(pulsar::core::EncodedPacket p) override { send(std::move(p)); }
    void on_audio(pulsar::core::AudioPacket) override {}
};

class MemoryPacketBuffer final : public pulsar::core::PacketBuffer {
    std::vector<uint8_t> data_;

public:
    explicit MemoryPacketBuffer(std::vector<uint8_t> data) : data_(std::move(data)) {}
    const uint8_t* data() const override { return data_.data(); }
    size_t size() const override { return data_.size(); }
};

// ─── SinkBroadcast: Sunshine-style persistent pipeline transport ───────────────
// The pipeline runs ONCE. Clients subscribe/unsubscribe without restarting.
// set_sink()  → fires Ready  → pipeline sends IDR then streams to new client
// clear_sink()→ fires Disconnected → pipeline suspends (waits for reconnect)
class SinkBroadcast final : public pulsar::core::ITransport {
    std::mutex                           mu_;
    std::shared_ptr<pulsar::core::ITransport> current_;
    std::function<void(pulsar::core::TransportEvent)> event_cb_;
    std::optional<pulsar::core::EncodedPacket> cached_keyframe_;

public:
    // Subscribe a new SSE client.
    // Just swap the sink — pipeline keeps running uninterrupted.
    // Browser receives next IDR (repeatSPSPPS=1 ensures every IDR has SPS+PPS).
    void set_sink(std::shared_ptr<pulsar::core::ITransport> sink) {
        {
            std::lock_guard lk(mu_);
            current_ = std::move(sink);
        }
        std::cerr << log_stamp() << "[broadcast] set_sink: event_cb=" << (bool)event_cb_ << "\n";
        if (event_cb_) event_cb_(pulsar::core::TransportEvent::Ready);

        std::optional<pulsar::core::EncodedPacket> keyframe;
        {
            std::lock_guard lk(mu_);
            keyframe = cached_keyframe_;
        }
        if (keyframe && current_) {
            std::cerr << log_stamp() << "[broadcast] replay cached keyframe size="
                      << (keyframe->buffer ? keyframe->buffer->size() : 0)
                      << " sink=" << current_->sink_id() << "\n";
            current_->send(std::move(*keyframe));
        }
    }

    // Unsubscribe if the given sink is still current.
    void clear_if_current(const pulsar::core::ITransport* expected) {
        std::cerr << log_stamp() << "[broadcast] clear_if_current called\n";
        {
            std::lock_guard lk(mu_);
            if (current_.get() != expected) { std::cerr << log_stamp() << "[broadcast] clear: not current, skip\n"; return; }
            current_ = nullptr;
        }
        // No event fired — pipeline continues running (suspended internally).
        // When next subscriber calls set_sink, pipeline resumes immediately.
    }

    bool has_sink() const {
        std::lock_guard<std::mutex> lk(const_cast<std::mutex&>(mu_));
        return current_ != nullptr;
    }

    std::shared_ptr<pulsar::core::ITransport> current_sink() const {
        std::lock_guard<std::mutex> lk(const_cast<std::mutex&>(mu_));
        return current_;
    }

    // ITransport — forwards packets to current subscriber
    void send(pulsar::core::EncodedPacket pkt) override {
        if (pkt.is_keyframe && pkt.buffer) {
            pulsar::core::EncodedPacket copy;
            copy.is_keyframe = pkt.is_keyframe;
            copy.pts_us      = pkt.pts_us;
            copy.codec       = pkt.codec;
            copy.buffer      = std::make_shared<MemoryPacketBuffer>(
                std::vector<uint8_t>(pkt.buffer->data(), pkt.buffer->data() + pkt.buffer->size()));
            std::lock_guard lk(mu_);
            cached_keyframe_ = std::move(copy);
        }

        std::shared_ptr<pulsar::core::ITransport> sink;
        { std::lock_guard lk(mu_); sink = current_; }
        if (!sink) return;
        static thread_local int logged = 0;
        if (logged < 12 || pkt.is_keyframe) {
            std::cerr << log_stamp() << "[broadcast] forward encoded size="
                      << (pkt.buffer ? pkt.buffer->size() : 0)
                      << " sink=" << sink->sink_id()
                      << " key=" << pkt.is_keyframe
                      << "\n";
            ++logged;
        }

        sink->send(std::move(pkt));
    }
    void send_batch(std::vector<pulsar::core::EncodedPacket> v) override {
        std::cerr << log_stamp() << "[broadcast] send_batch count=" << v.size() << "\n";
        for (auto& p : v) send(std::move(p));
    }
    void send_audio(pulsar::core::AudioPacket pkt) override {
        std::shared_ptr<pulsar::core::ITransport> sink;
        { std::lock_guard lk(mu_); sink = current_; }
        if (!sink) return;
        static thread_local int logged = 0;
        if (logged < 8) {
            std::cerr << log_stamp() << "[broadcast] forward audio size=" << pkt.size << " sink=" << sink->sink_id() << "\n";
            ++logged;
        }
        sink->send_audio(std::move(pkt));
    }
    void on_packet(pulsar::core::EncodedPacket p) override { send(std::move(p)); }
    void on_audio(pulsar::core::AudioPacket p) override { send_audio(std::move(p)); }
    bool connect(const std::string&) override { return true; }
    void disconnect() override {}
    void set_event_callback(std::function<void(pulsar::core::TransportEvent)> cb) override {
        event_cb_ = std::move(cb);
        // Fire Ready so pipeline starts immediately
        if (event_cb_) event_cb_(pulsar::core::TransportEvent::Ready);
    }
    void set_stats_callback(std::function<void(pulsar::core::NetworkStats)>) override {}
    void set_input_callback(std::function<void(pulsar::core::InputEvent)> cb) override {}
    void set_mic_callback(std::function<void(pulsar::core::AudioFrame)>) override {}
    void set_jitter_buffer(int, int) override {}
    void set_fec_params(const pulsar::core::FecParams&) override {}
    void send_haptic(const pulsar::core::HapticCommand&) override {}
    void send_stats(const pulsar::core::PipelineMetrics&) override {}
    std::string sink_id() const override { return "broadcast"; }
};

static std::string request_path(const std::string& req) {
    const auto first_space = req.find(' ');
    if (first_space == std::string::npos) return {};
    const auto second_space = req.find(' ', first_space + 1);
    if (second_space == std::string::npos || second_space <= first_space + 1) return {};
    return req.substr(first_space + 1, second_space - first_space - 1);
}

static std::string request_path_only(const std::string& req) {
    const auto path = request_path(req);
    const auto qpos = path.find('?');
    return qpos == std::string::npos ? path : path.substr(0, qpos);
}

static std::string request_query_param(const std::string& req, const std::string& key) {
    const auto path = request_path(req);
    const auto qpos = path.find('?');
    if (qpos == std::string::npos) return {};
    const auto query = path.substr(qpos + 1);
    size_t pos = 0;
    while (pos < query.size()) {
        const auto amp = query.find('&', pos);
        const auto pair = query.substr(pos, amp == std::string::npos ? std::string::npos : amp - pos);
        const auto eq = pair.find('=');
        if (eq != std::string::npos && pair.substr(0, eq) == key) {
            return pair.substr(eq + 1);
        }
        if (amp == std::string::npos) break;
        pos = amp + 1;
    }
    return {};
}

class StreamSessionBroker final {
public:
    StreamSessionBroker(pulsar::core::ISessionManager& registry,
                        std::shared_ptr<SinkBroadcast> broadcast,
                        StdoutLogger& logger,
                        int max_clients)
        : registry_(registry),
          broadcast_(std::move(broadcast)),
          logger_(logger),
          max_clients_(std::max(1, max_clients)) {}

    void attach(const std::string& sid,
                const std::shared_ptr<pulsar::core::ITransport>& transport) {
        if (!transport || sid.empty()) return;

        std::shared_ptr<pulsar::core::ITransport> old_transport;
        std::string previous_sid;
        {
            std::lock_guard lk(mu_);
            previous_sid = active_sid_;
            const bool replacing_existing = records_.find(sid) != records_.end();
            if (!replacing_existing) {
                evict_if_needed_locked(sid);
            }

            auto now = std::chrono::steady_clock::now();
            auto& rec = ensure_record_locked(sid);
            rec.last_seen = now;
            rec.state = pulsar::core::SessionState::Active;
            rec.generation += 1;
            rec.transport = transport;

            if (!previous_sid.empty() && previous_sid != sid) {
                if (auto prev = records_.find(previous_sid); prev != records_.end()) {
                    prev->second.state = pulsar::core::SessionState::Suspended;
                    prev->second.last_seen = now;
                    prev->second.transport.reset();
                }
            }

            active_sid_ = sid;
            old_transport = broadcast_->current_sink();
        }

        logger_.log(pulsar::core::LogLevel::Info,
                    "session: attach sid=" + sid +
                    " prev_sid=" + (previous_sid.empty() ? "<none>" : previous_sid) +
                    " had_old_transport=" + std::string(old_transport ? "1" : "0"));

        if (old_transport && old_transport.get() != transport.get()) {
            if (previous_sid != sid) {
                if (auto old_sse = std::dynamic_pointer_cast<SseTransport>(old_transport)) {
                    old_sse->preempt();
                }
            }
            old_transport->disconnect();
        }

        broadcast_->set_sink(transport);

        pulsar::core::AuthToken tok;
        tok.scheme = "session";
        tok.data["username"] = sid;
        if (auto* s = registry_.find(sid)) {
            s->state = pulsar::core::SessionState::Active;
        } else {
            (void)registry_.create(tok);
            registry_.activate(sid);
        }

        logger_.log(pulsar::core::LogLevel::Info, "session: attached sid=" + sid);
    }

    void detach(const std::string& sid, const pulsar::core::ITransport* expected) {
        std::lock_guard lk(mu_);
        auto it = records_.find(sid);
        if (it == records_.end()) return;

        auto& rec = it->second;
        rec.last_seen = std::chrono::steady_clock::now();
        rec.state = pulsar::core::SessionState::Suspended;
        if (rec.transport && rec.transport.get() == expected) {
            broadcast_->clear_if_current(expected);
            rec.transport.reset();
        }
        if (active_sid_ == sid) active_sid_.clear();
        if (auto* s = registry_.find(sid)) {
            s->state = pulsar::core::SessionState::Suspended;
        }
        logger_.log(pulsar::core::LogLevel::Info,
                    "session: detach sid=" + sid +
                    " active_sid=" + (active_sid_.empty() ? "<none>" : active_sid_));
    }

    bool is_active(const std::string& sid) const {
        std::lock_guard lk(mu_);
        return !active_sid_.empty() && active_sid_ == sid;
    }

    void force_active(const std::string& sid) {
        if (sid.empty()) return;
        std::lock_guard lk(mu_);
        auto& rec = ensure_record_locked(sid);
        rec.last_seen = std::chrono::steady_clock::now();
        rec.state = pulsar::core::SessionState::Active;
        active_sid_ = sid;
        if (auto* s = registry_.find(sid)) {
            s->state = pulsar::core::SessionState::Active;
        }
    }

private:
    struct Record {
        pulsar::core::SessionState state = pulsar::core::SessionState::Authenticating;
        std::shared_ptr<pulsar::core::ITransport> transport;
        std::chrono::steady_clock::time_point last_seen = std::chrono::steady_clock::now();
        std::uint64_t generation = 0;
    };

    Record& ensure_record_locked(const std::string& sid) {
        auto [it, inserted] = records_.try_emplace(sid);
        if (inserted) {
            pulsar::core::AuthToken tok;
            tok.scheme = "session";
            tok.data["username"] = sid;
            (void)registry_.create(tok);
        }
        return it->second;
    }

    void evict_if_needed_locked(const std::string& incoming_sid) {
        if (static_cast<int>(records_.size()) < max_clients_ || records_.find(incoming_sid) != records_.end()) {
            return;
        }

        std::string victim;
        std::chrono::steady_clock::time_point oldest;
        bool have_victim = false;

        for (const auto& [sid, rec] : records_) {
            if (sid == active_sid_) continue;
            if (!have_victim || rec.last_seen < oldest) {
                victim = sid;
                oldest = rec.last_seen;
                have_victim = true;
            }
        }

        if (!have_victim) {
            logger_.log(pulsar::core::LogLevel::Warn,
                        "session: max_clients reached, keeping active session and delaying eviction");
            return;
        }

        records_.erase(victim);
        registry_.terminate(victim);
        logger_.log(pulsar::core::LogLevel::Info, "session: evicted sid=" + victim);
    }

    mutable std::mutex mu_;
    pulsar::core::ISessionManager& registry_;
    std::shared_ptr<SinkBroadcast> broadcast_;
    StdoutLogger& logger_;
    int max_clients_;
    std::unordered_map<std::string, Record> records_;
    std::string active_sid_;
};

// ─── JS key code → Linux evdev code mapping ───────────────────────────────────
static int js_code_to_linux(const std::string& code) {
    static const std::unordered_map<std::string, int> kMap = {
        {"KeyA",KEY_A},{"KeyB",KEY_B},{"KeyC",KEY_C},{"KeyD",KEY_D},{"KeyE",KEY_E},
        {"KeyF",KEY_F},{"KeyG",KEY_G},{"KeyH",KEY_H},{"KeyI",KEY_I},{"KeyJ",KEY_J},
        {"KeyK",KEY_K},{"KeyL",KEY_L},{"KeyM",KEY_M},{"KeyN",KEY_N},{"KeyO",KEY_O},
        {"KeyP",KEY_P},{"KeyQ",KEY_Q},{"KeyR",KEY_R},{"KeyS",KEY_S},{"KeyT",KEY_T},
        {"KeyU",KEY_U},{"KeyV",KEY_V},{"KeyW",KEY_W},{"KeyX",KEY_X},{"KeyY",KEY_Y},
        {"KeyZ",KEY_Z},
        {"Digit1",KEY_1},{"Digit2",KEY_2},{"Digit3",KEY_3},{"Digit4",KEY_4},
        {"Digit5",KEY_5},{"Digit6",KEY_6},{"Digit7",KEY_7},{"Digit8",KEY_8},
        {"Digit9",KEY_9},{"Digit0",KEY_0},
        {"Minus",KEY_MINUS},{"Equal",KEY_EQUAL},{"Backspace",KEY_BACKSPACE},
        {"Tab",KEY_TAB},{"BracketLeft",KEY_LEFTBRACE},{"BracketRight",KEY_RIGHTBRACE},
        {"Enter",KEY_ENTER},{"ControlLeft",KEY_LEFTCTRL},{"ControlRight",KEY_RIGHTCTRL},
        {"Semicolon",KEY_SEMICOLON},{"Quote",KEY_APOSTROPHE},{"Backquote",KEY_GRAVE},
        {"ShiftLeft",KEY_LEFTSHIFT},{"ShiftRight",KEY_RIGHTSHIFT},{"Backslash",KEY_BACKSLASH},
        {"Comma",KEY_COMMA},{"Period",KEY_DOT},{"Slash",KEY_SLASH},{"CapsLock",KEY_CAPSLOCK},
        {"F1",KEY_F1},{"F2",KEY_F2},{"F3",KEY_F3},{"F4",KEY_F4},{"F5",KEY_F5},
        {"F6",KEY_F6},{"F7",KEY_F7},{"F8",KEY_F8},{"F9",KEY_F9},{"F10",KEY_F10},
        {"F11",KEY_F11},{"F12",KEY_F12},
        {"PrintScreen",KEY_SYSRQ},{"ScrollLock",KEY_SCROLLLOCK},{"Pause",KEY_PAUSE},
        {"Insert",KEY_INSERT},{"Home",KEY_HOME},{"PageUp",KEY_PAGEUP},
        {"Delete",KEY_DELETE},{"End",KEY_END},{"PageDown",KEY_PAGEDOWN},
        {"ArrowRight",KEY_RIGHT},{"ArrowLeft",KEY_LEFT},{"ArrowDown",KEY_DOWN},{"ArrowUp",KEY_UP},
        {"NumLock",KEY_NUMLOCK},{"NumpadDivide",KEY_KPSLASH},{"NumpadMultiply",KEY_KPASTERISK},
        {"NumpadSubtract",KEY_KPMINUS},{"NumpadAdd",KEY_KPPLUS},{"NumpadEnter",KEY_KPENTER},
        {"Numpad0",KEY_KP0},{"Numpad1",KEY_KP1},{"Numpad2",KEY_KP2},{"Numpad3",KEY_KP3},
        {"Numpad4",KEY_KP4},{"Numpad5",KEY_KP5},{"Numpad6",KEY_KP6},{"Numpad7",KEY_KP7},
        {"Numpad8",KEY_KP8},{"Numpad9",KEY_KP9},{"NumpadDecimal",KEY_KPDOT},
        {"AltLeft",KEY_LEFTALT},{"AltRight",KEY_RIGHTALT},
        {"Space",KEY_SPACE},{"Escape",KEY_ESC},
        {"MetaLeft",KEY_LEFTMETA},{"MetaRight",KEY_RIGHTMETA},{"ContextMenu",KEY_COMPOSE},
    };
    auto it = kMap.find(code);
    return it != kMap.end() ? it->second : 0;
}

// ─── WebRTC session loop (TCP signaling) ───────────────────────────────────────
// Minimal signaling protocol over a plain TCP socket:
//   1. Server sends SDP offer followed by "\r\n---\r\n"
//   2. Client sends SDP answer followed by "\r\n---\r\n"
//   3. ICE negotiates; media flows
static void run_webrtc_loop(const ServerConfig& cfg, StdoutLogger& logger,
                             std::atomic<bool>& stop,
                             pulsar::core::IInputHandler& input)
{
    int srv_fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (srv_fd < 0) {
        logger.log(pulsar::core::LogLevel::Error, "webrtc: signaling socket failed");
        return;
    }
    int opt = 1;
    ::setsockopt(srv_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
#ifdef SO_REUSEPORT
    ::setsockopt(srv_fd, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt));
#endif
    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port        = htons(static_cast<uint16_t>(cfg.protocols.webrtc.port));
    if (::bind(srv_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        logger.log(pulsar::core::LogLevel::Error,
            "webrtc: signaling bind failed on port " +
            std::to_string(cfg.protocols.webrtc.port) +
            " (" + std::string(std::strerror(errno)) + ")");
        ::close(srv_fd);
        return;
    }
    ::listen(srv_fd, 5);
    logger.log(pulsar::core::LogLevel::Info,
        "  listener: webrtc signaling port=" +
        std::to_string(cfg.protocols.webrtc.port) +
        "  (browser: http://<server_ip>:" +
        std::to_string(cfg.protocols.webrtc.port) + "/)");

    // Embedded test HTML page
    static const char kHtmlPage[] = R"html(<!DOCTYPE html>
<html><head><meta charset="utf-8"><title>Pulsar Stream</title>
<style>
body{background:#111;color:#eee;font-family:sans-serif;text-align:center;margin:0;padding:16px}
canvas,video{width:100%;max-width:960px;background:#000;border:1px solid #333;display:block;margin:8px auto}
button{margin:4px;padding:8px 18px;font-size:.95rem;cursor:pointer;border:none;border-radius:4px;background:#444;color:#fff}
button:hover{background:#666}
#status{max-width:960px;margin:8px auto;padding:10px 12px;border:1px solid #4b4b4b;border-radius:6px;background:#181818;color:#d7ffd7;text-align:left;font-size:.9rem;min-height:1.4em}
#log{text-align:left;max-width:960px;margin:8px auto;font-size:.75rem;color:#888;height:80px;overflow-y:auto;border:1px solid #333;padding:4px}
.section{max-width:960px;margin:8px auto;padding:8px;border:1px solid #333;border-radius:4px;text-align:left}
h3{margin:4px 0;font-size:.9rem;color:#aaa}
</style></head><body>
<h2 style="margin:8px 0">Pulsar HTTP Stream</h2>

<div class="section">
<h3>Option A — HTTP Video Stream (works through SSH tunnel, no UDP needed)</h3>
<button onclick="startSSE()">▶ Start HTTP Stream</button>
<button onclick="stopSSE()">■ Stop</button>
<canvas id="canvas" tabindex="0"></canvas>
</div>

<div id="status">Idle. Start HTTP Stream, then click the canvas to arm input.</div>
<div id="log"></div>
<script>
let es, dec;
function log(s){const el=document.getElementById('log');el.textContent+=s+'\n';el.scrollTop=el.scrollHeight;}
function setStatus(s){document.getElementById('status').textContent=s;}

let sessionId = (() => {
    const key = 'pulsar.session_id';
    const fromStorage = localStorage.getItem(key);
    if (fromStorage) return fromStorage;
    const generated = (crypto.randomUUID ? crypto.randomUUID() : ('s-' + Date.now() + '-' + Math.random().toString(16).slice(2)));
    localStorage.setItem(key, generated);
    return generated;
})();
function withSid(path){
    const u = new URL(path, window.location.origin);
    u.searchParams.set('sid', sessionId);
    return u.pathname + u.search + u.hash;
}

// ── Input injection ────────────────────────────────────────────────────────────
let videoW=1920,videoH=1080,lastMX=-1,lastMY=-1;
let inputEnabled=false;
function releaseInputCapture(){
    inputEnabled=false;
    lastMX=-1;
    lastMY=-1;
    setStatus('Input released to local browser. Click canvas to re-arm.');
    log('Input released by shortcut.');
}
function sendInput(d){if(!inputEnabled)return;setStatus('Input: '+d.type+(d.type==='mb' ? (' button '+d.btn+' '+(d.down ? 'down' : 'up')) : '')+(d.type==='mm' || d.type==='ma' ? (d.type==='mm' ? (' dx='+d.dx+' dy='+d.dy) : (' x='+d.x+' y='+d.y)) : '')+(d.type==='kd' || d.type==='ku' ? (' '+d.code) : ''));fetch(withSid('/input'),{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(d)}).catch(()=>{});}
let lastCX=-1,lastCY=-1;
function sendAbsolutePointer(e){const r=canvas.getBoundingClientRect();if(!r.width)return;const x=Math.round((e.clientX-r.left)/r.width*videoW);const y=Math.round((e.clientY-r.top)/r.height*videoH);const ax=Math.max(0,Math.min(65535,Math.round((x/Math.max(1,videoW))*65535)));const ay=Math.max(0,Math.min(65535,Math.round((y/Math.max(1,videoH))*65535)));sendInput({type:'ma',x:ax,y:ay});lastCX=e.clientX;lastCY=e.clientY;lastMX=x;lastMY=y;setStatus('Pointer '+x+','+y+' on '+videoW+'x'+videoH);}
const canvas=document.getElementById('canvas');
canvas.addEventListener('pointerdown',e=>{if(!es)return;if(!inputEnabled){e.preventDefault();canvas.focus();inputEnabled=true;log('Input armed.');setStatus('Input armed. Mouse and keyboard will now be sent.');}try{canvas.setPointerCapture(e.pointerId);}catch(_e){}lastCX=e.clientX;lastCY=e.clientY;sendAbsolutePointer(e);sendInput({type:'mb',btn:e.button+1,down:1});});
canvas.addEventListener('pointermove',e=>{if(!es || !inputEnabled)return;sendAbsolutePointer(e);});
function releaseMouseButton(e){if(!es || !inputEnabled)return;sendInput({type:'mb',btn:e.button+1,down:0});setStatus('Mouse button '+(e.button+1)+' released');}
canvas.addEventListener('pointerup',e=>{releaseMouseButton(e);try{canvas.releasePointerCapture(e.pointerId);}catch(_e){}});
canvas.addEventListener('pointercancel',e=>{releaseMouseButton(e);try{canvas.releasePointerCapture(e.pointerId);}catch(_e){}});
document.addEventListener('mouseup',releaseMouseButton);
canvas.addEventListener('contextmenu',e=>e.preventDefault());
document.addEventListener('wheel',e=>{if(!es || !inputEnabled)return;e.preventDefault();sendInput({type:'mw',delta:e.deltaY>0?-1:1});setStatus('Mouse wheel '+(e.deltaY>0?'down':'up'));},{passive:false});
function isRemoteExitShortcut(e){return e.ctrlKey && e.code==='Backquote';}
document.addEventListener('keydown',e=>{if(isRemoteExitShortcut(e)){e.preventDefault();e.stopPropagation();releaseInputCapture();return;}if(!es || !inputEnabled)return;e.preventDefault();sendInput({type:'kd',code:e.code});setStatus('Key down '+e.code);});
document.addEventListener('keyup',e=>{if(isRemoteExitShortcut(e)){e.preventDefault();e.stopPropagation();return;}if(!es || !inputEnabled)return;e.preventDefault();sendInput({type:'ku',code:e.code});setStatus('Key up '+e.code);});

// ── Option A: SSE + WebCodecs ──────────────────────────────────────────────
function stopSSE(){
  if(es){es.close();es=null;}
    if(dec){
        try{ if(dec.state!=='closed') dec.close(); }catch(_e){}
        dec=null;
    }
    inputEnabled=false;
  lastMX=-1;lastMY=-1;
  const ctx=canvas.getContext('2d');ctx.clearRect(0,0,canvas.width,canvas.height);
  log('Stream stopped.');
}

const autoStartHttp = window.location.search.includes('autostart');

async function startSSE(){
  stopSSE();
  if(typeof VideoDecoder==='undefined'){log('WebCodecs not supported \u2014 need Chrome/Edge 94+');return;}
        sessionId = (crypto.randomUUID ? crypto.randomUUID() : ('s-' + Date.now() + '-' + Math.random().toString(16).slice(2)));
        localStorage.setItem('pulsar.session_id', sessionId);
        setStatus('Connecting stream for session '+sessionId.slice(0,8)+'...');
  const ctx=canvas.getContext('2d');
    const t0=performance.now();
    const mark=(msg)=>log('+'+Math.round(performance.now()-t0)+'ms '+msg);
  let configured=false, ts=0;
    let firstFrameLogged=false;
    let pendingKeyFrame=null;

    function flushPendingKeyFrame(){
        if(!pendingKeyFrame || !configured || !dec || dec.state!=='configured') return;
        const chunkData=annexBToAvcc(pendingKeyFrame);
        if(!chunkData) return;
        const type='key';
        const data=chunkData;
        pendingKeyFrame=null;
        try{
            dec.decode(new EncodedVideoChunk({type,timestamp:ts+=33333,data}));
        }catch(err){mark('decode: '+err.message);}
    }

  dec=new VideoDecoder({
    output: frame=>{
      if(canvas.width!==frame.displayWidth||canvas.height!==frame.displayHeight){
        canvas.width=frame.displayWidth; canvas.height=frame.displayHeight;
        videoW=frame.displayWidth; videoH=frame.displayHeight;
      }
                                                if(!firstFrameLogged){mark('first frame rendered'); firstFrameLogged=true;}
      ctx.drawImage(frame,0,0);
      frame.close();
    },
    error: e=>log('decode error: '+e.message)
  });

    mark('Connecting...');
    es=new EventSource(withSid('/stream'));
    es.onerror=()=>{mark('SSE disconnected');setStatus('Stream disconnected.');};
  // Server sends event:stop when this session is preempted by a newer one.
  // Calling es.close() here prevents EventSource auto-reconnect cascade.
  es.addEventListener('stop',()=>{
    if(es){es.close();es=null;}
        mark('Session closed by server.');
      setStatus('Session closed by server.');
  });

  es.addEventListener('sps_pps', e=>{
    if(configured) return;
    const raw=Uint8Array.from(atob(e.data),c=>c.charCodeAt(0));
    configureDec(raw);
        flushPendingKeyFrame();
  });

    function extractAnnexBUnits(raw){
        const units=[];
        let i=0;
        while(i+4<raw.length){
            let start=-1;
            let startLen=0;
            for(let j=i;j+3<raw.length;++j){
                if(raw[j]===0&&raw[j+1]===0&&raw[j+2]===0&&raw[j+3]===1){start=j+4; startLen=4; break;}
                if(j+2<raw.length&&raw[j]===0&&raw[j+1]===0&&raw[j+2]===1){start=j+3; startLen=3; break;}
            }
            if(start<0) break;
            let end=raw.length;
            for(let j=start;j+3<raw.length;++j){
                if(raw[j]===0&&raw[j+1]===0&&raw[j+2]===0&&raw[j+3]===1){end=j; break;}
                if(j+2<raw.length&&raw[j]===0&&raw[j+1]===0&&raw[j+2]===1){end=j; break;}
            }
            if(end>start) units.push(raw.slice(start,end));
            i=end;
            if(startLen===0) break;
        }
        return units;
    }

    function buildAvcc(raw){
        const units=extractAnnexBUnits(raw);
        const sps=units.find(u => (u[0] & 0x1f) === 7);
        const pps=units.find(u => (u[0] & 0x1f) === 8);
        if(!sps || !pps || sps.length < 4) return null;
        const out=new Uint8Array(11 + sps.length + pps.length);
        let o=0;
        out[o++]=1;
        out[o++]=sps[1];
        out[o++]=sps[2];
        out[o++]=sps[3];
        out[o++]=0xfc | 3;
        out[o++]=0xe0 | 1;
        out[o++]=(sps.length>>>8)&255; out[o++]=sps.length&255;
        out.set(sps, o); o+=sps.length;
        out[o++]=1;
        out[o++]=(pps.length>>>8)&255; out[o++]=pps.length&255;
        out.set(pps, o);
        return { description: out, codec: 'avc1.' + sps[1].toString(16).padStart(2,'0') + sps[2].toString(16).padStart(2,'0') + sps[3].toString(16).padStart(2,'0') };
    }

    function annexBToAvcc(raw){
        const units=extractAnnexBUnits(raw).filter(u => u.length > 0);
        let total=0;
        for(const unit of units) total += 4 + unit.length;
        if(total===0) return null;
        const out=new Uint8Array(total);
        let o=0;
        for(const unit of units){
            const len=unit.length;
            out[o++]=(len>>>24)&255;
            out[o++]=(len>>>16)&255;
            out[o++]=(len>>>8)&255;
            out[o++]=len&255;
            out.set(unit, o);
            o += len;
        }
        return out;
    }

  function configureDec(raw){
    if(configured) return;
        const cfg=buildAvcc(raw);
        if(!cfg){
            mark('configure failed: missing SPS/PPS');
            return;
        }
        mark('codec: '+cfg.codec+' avcc='+cfg.description.length+' bytes');
        try{
            // WebCodecs is most reliable when we hand it the AVCDecoderConfigurationRecord.
            dec.configure({codec: cfg.codec, description: cfg.description, hardwareAcceleration:'prefer-software'});
            configured=true;
            mark('decoder ready \u2014 click canvas to enable input');
            setStatus('Decoder ready. Click canvas once to arm input.');
                        flushPendingKeyFrame();
        }catch(err){
            try{
                dec.configure({codec: cfg.codec, hardwareAcceleration:'prefer-software'});
                configured=true;
                mark('decoder ready (codec only fallback)');
                setStatus('Decoder ready. Click canvas once to arm input.');
                                flushPendingKeyFrame();
            }catch(e2){mark('configure failed: '+e2.message);}
        }
  }

  const feed=(type,e)=>{
    const data=Uint8Array.from(atob(e.data),c=>c.charCodeAt(0));
    // If not yet configured, try to extract SPS from this keyframe
        if(type==='key' && !pendingKeyFrame) pendingKeyFrame=data;
        if(!configured && type==='key') configureDec(data);
        if(!configured||!dec||dec.state!=='configured') return;
        const chunkData=annexBToAvcc(data);
        if(!chunkData) return;
    try{
            dec.decode(new EncodedVideoChunk({type,timestamp:ts+=33333,data:chunkData}));
        }catch(err){mark('decode: '+err.message);}
  };
  es.addEventListener('key',  e=>feed('key',e));
  es.addEventListener('delta',e=>feed('delta',e));
}

if (autoStartHttp) {
    setTimeout(() => startSSE().catch(err => log('auto-start failed: ' + err.message)), 0);
}

</script></body></html>)html";

        static const char kWebRtcPage[] = R"html(<!DOCTYPE html>
<html><head><meta charset="utf-8"><title>Pulsar WebRTC Test</title>
<style>
body{background:linear-gradient(180deg,#0c111b,#111);color:#e8edf5;font-family:system-ui,sans-serif;margin:0;padding:24px}
.wrap{max-width:980px;margin:0 auto}
.card{background:rgba(17,24,39,.88);border:1px solid #263248;border-radius:16px;padding:20px;box-shadow:0 18px 48px rgba(0,0,0,.35)}
h1{margin:0 0 6px;font-size:28px}
p{margin:8px 0;color:#b8c0d4;line-height:1.5}
button{appearance:none;border:0;border-radius:999px;padding:12px 18px;margin:6px 8px 6px 0;background:#4f7cff;color:#fff;font-weight:700;cursor:pointer}
button.secondary{background:#243149}
button:disabled{opacity:.55;cursor:not-allowed}
#status{margin-top:14px;padding:12px 14px;border-radius:12px;background:#0f1726;border:1px solid #243149;min-height:1.4em}
#log{margin-top:10px;height:190px;overflow:auto;padding:12px;border-radius:12px;background:#0b101a;border:1px solid #243149;font-family:ui-monospace, SFMono-Regular, Menlo, monospace;font-size:13px;white-space:pre-wrap}
.viewport{position:relative;margin-top:16px;max-width:960px}
video{display:block;width:100%;background:#000;border-radius:16px;border:1px solid #243149;outline:none;cursor:crosshair}
.overlay{position:absolute;inset:0;display:flex;align-items:flex-start;justify-content:flex-start;pointer-events:none;padding:12px}
.badge{pointer-events:none;background:rgba(15,23,42,.8);color:#dbeafe;border:1px solid #334155;border-radius:999px;padding:6px 10px;font-size:12px;backdrop-filter:blur(8px)}
.row{display:flex;flex-wrap:wrap;align-items:center}
.hint{color:#8ea0c2;font-size:14px}
</style></head><body>
<div class="wrap">
    <div class="card">
        <h1>Pulsar WebRTC Test</h1>
        <p class="hint">Click the button below to send an SDP offer to <code>/offer</code> and attach the returned video track to this page. Click the video once to arm input, then use mouse and keyboard normally. Ctrl+` releases input back to the browser.</p>
        <div class="row">
            <button id="startBtn">Start WebRTC</button>
            <button id="stopBtn" class="secondary" disabled>Stop</button>
            <button id="armBtn" class="secondary" disabled>Arm Input</button>
            <button id="releaseBtn" class="secondary" disabled>Release Input</button>
            <button id="copyBtn" class="secondary">Copy Session ID</button>
            <button id="openHomeBtn" class="secondary">Open HTTP Stream</button>
        </div>
        <div id="status">Idle.</div>
        <div id="log"></div>
        <div class="viewport">
            <video id="remoteVideo" autoplay playsinline tabindex="0"></video>
            <div class="overlay"><div id="inputBadge" class="badge">Input disabled</div></div>
        </div>
    </div>
</div>
<script>
let pc = null;
let remoteStream = null;
let starting = false;
let inputEnabled = false;
let videoW = 1920;
let videoH = 1080;
let lastMX = -1;
let lastMY = -1;
let sessionId = (() => {
    const key = 'pulsar.webrtc_session_id';
    const stored = localStorage.getItem(key);
    if (stored) return stored;
    const generated = (crypto.randomUUID ? crypto.randomUUID() : ('w-' + Date.now() + '-' + Math.random().toString(16).slice(2)));
    localStorage.setItem(key, generated);
    return generated;
})();

const statusEl = document.getElementById('status');
const logEl = document.getElementById('log');
const remoteVideo = document.getElementById('remoteVideo');
const startBtn = document.getElementById('startBtn');
const stopBtn = document.getElementById('stopBtn');
const armBtn = document.getElementById('armBtn');
const releaseBtn = document.getElementById('releaseBtn');
const copyBtn = document.getElementById('copyBtn');
const openHomeBtn = document.getElementById('openHomeBtn');
const inputBadge = document.getElementById('inputBadge');

function setStatus(text) { statusEl.textContent = text; }
function setInputBadge(text) { inputBadge.textContent = text; }
function log(text) {
    const line = '[' + new Date().toLocaleTimeString() + '] ' + text;
    logEl.textContent += line + '\n';
    logEl.scrollTop = logEl.scrollHeight;
}
function withSid(path) {
    const url = new URL(path, window.location.origin);
    url.searchParams.set('sid', sessionId);
    return url.pathname + url.search + url.hash;
}

function waitForIceGatheringComplete(peerConnection) {
    if (peerConnection.iceGatheringState === 'complete') {
        return Promise.resolve();
    }
    return new Promise((resolve) => {
        const onStateChange = () => {
            if (peerConnection.iceGatheringState === 'complete') {
                peerConnection.removeEventListener('icegatheringstatechange', onStateChange);
                resolve();
            }
        };
        peerConnection.addEventListener('icegatheringstatechange', onStateChange);
    });
}

function updateInputState(enabled) {
    inputEnabled = enabled;
    armBtn.disabled = enabled || !pc;
    releaseBtn.disabled = !enabled;
    setInputBadge(enabled ? 'Input armed' : 'Input disabled');
    setStatus(enabled ? 'Input armed. Click/drag on video and use keyboard.' : 'Input disabled. Click Arm Input or click the video.');
}

function releaseInputCapture() {
    updateInputState(false);
    lastMX = -1;
    lastMY = -1;
    log('input released');
}

function sendInput(payload) {
    if (!inputEnabled) return;
    fetch(withSid('/input'), {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify(payload),
    }).catch(() => {});
}

function sendAbsolutePointer(event) {
    const rect = remoteVideo.getBoundingClientRect();
    if (!rect.width || !rect.height) return;
    const x = Math.round((event.clientX - rect.left) / rect.width * videoW);
    const y = Math.round((event.clientY - rect.top) / rect.height * videoH);
    const ax = Math.max(0, Math.min(65535, Math.round((x / Math.max(1, videoW)) * 65535)));
    const ay = Math.max(0, Math.min(65535, Math.round((y / Math.max(1, videoH)) * 65535)));
    sendInput({ type: 'ma', x: ax, y: ay });
    lastMX = x;
    lastMY = y;
    setStatus('Pointer ' + x + ',' + y + ' on ' + videoW + 'x' + videoH);
}

function isRemoteExitShortcut(event) {
    return event.ctrlKey && event.code === 'Backquote';
}

async function stopWebRtc() {
    if (pc) {
        try { pc.close(); } catch (_e) {}
        pc = null;
    }
    if (remoteStream) {
        remoteStream.getTracks().forEach((track) => track.stop());
        remoteStream = null;
    }
    remoteVideo.srcObject = null;
    updateInputState(false);
    startBtn.disabled = false;
    stopBtn.disabled = true;
    setStatus('Stopped.');
}

function redirectToHttpStream() {
    localStorage.setItem('pulsar.session_id', sessionId);
    window.location.href = '/?autostart=1';
}

async function startWebRtc() {
    if (starting) {
        log('start ignored: already starting');
        return;
    }
    starting = true;
    await stopWebRtc();
    startBtn.disabled = true;
    setStatus('Creating peer connection...');
    log('starting session ' + sessionId.slice(0, 8));

    pc = new RTCPeerConnection();
    remoteStream = new MediaStream();
    remoteVideo.srcObject = remoteStream;

    pc.ontrack = (event) => {
        remoteStream.addTrack(event.track);
        log('track: ' + event.track.kind);
        setStatus('Receiving ' + event.track.kind + '...');
        if (event.track.kind === 'video') {
            const settings = event.track.getSettings ? event.track.getSettings() : {};
            if (settings.width) videoW = settings.width;
            if (settings.height) videoH = settings.height;
        }
    };
    pc.oniceconnectionstatechange = () => log('ice: ' + pc.iceConnectionState);
    pc.onconnectionstatechange = () => {
        log('pc: ' + pc.connectionState);
        if (pc.connectionState === 'connected') {
            setStatus('Connected.');
            armBtn.disabled = false;
        } else if (pc.connectionState === 'failed' || pc.connectionState === 'disconnected') {
            setStatus('Connection ' + pc.connectionState + '.');
        }
    };

    pc.addTransceiver('video', { direction: 'recvonly' });

    const offer = await pc.createOffer();
    await pc.setLocalDescription(offer);
    log('offer created (' + offer.sdp.length + ' bytes)');
    setStatus('Gathering ICE candidates...');
    await waitForIceGatheringComplete(pc);
    log('ice gathering complete');

    setStatus('Sending offer to /offer ...');
    const response = await fetch(withSid('/offer'), {
        method: 'POST',
        headers: { 'Content-Type': 'application/sdp' },
        body: offer.sdp,
    });

    if (!response.ok) {
        throw new Error('offer failed: HTTP ' + response.status);
    }

    const answerSdp = await response.text();
    log('answer received (' + answerSdp.length + ' bytes)');
    await pc.setRemoteDescription({ type: 'answer', sdp: answerSdp });

    if (!pc || pc.connectionState !== 'connected') {
        log('WebRTC media is not reachable in this workspace; switching to HTTP stream');
        setStatus('Switching to the working HTTP stream...');
        redirectToHttpStream();
        return;
    }

    startBtn.disabled = true;
    stopBtn.disabled = false;
    setStatus('Answer applied. Waiting for ICE/DTLS...');
    armBtn.disabled = false;
    starting = false;
}

startBtn.addEventListener('click', async () => {
    try {
        await startWebRtc();
    } catch (err) {
        log('error: ' + err.message);
        setStatus('Error: ' + err.message);
        await stopWebRtc();
        starting = false;
    }
});

stopBtn.addEventListener('click', async () => {
    await stopWebRtc();
    starting = false;
});

armBtn.addEventListener('click', () => {
    if (!pc) return;
    remoteVideo.focus();
    updateInputState(true);
    log('input armed');
});

releaseBtn.addEventListener('click', () => {
    releaseInputCapture();
});

remoteVideo.addEventListener('loadedmetadata', () => {
    if (remoteVideo.videoWidth) videoW = remoteVideo.videoWidth;
    if (remoteVideo.videoHeight) videoH = remoteVideo.videoHeight;
});

remoteVideo.addEventListener('pointerdown', (event) => {
    if (!pc) return;
    if (!inputEnabled) {
        event.preventDefault();
        remoteVideo.focus();
        updateInputState(true);
        log('input armed');
    }
    try { remoteVideo.setPointerCapture(event.pointerId); } catch (_e) {}
    sendAbsolutePointer(event);
    sendInput({ type: 'mb', btn: event.button + 1, down: 1 });
});

remoteVideo.addEventListener('pointermove', (event) => {
    if (!pc || !inputEnabled) return;
    sendAbsolutePointer(event);
});

function releaseMouseButton(event) {
    if (!pc || !inputEnabled) return;
    sendInput({ type: 'mb', btn: event.button + 1, down: 0 });
}

remoteVideo.addEventListener('pointerup', (event) => {
    releaseMouseButton(event);
    try { remoteVideo.releasePointerCapture(event.pointerId); } catch (_e) {}
});

remoteVideo.addEventListener('pointercancel', (event) => {
    releaseMouseButton(event);
    try { remoteVideo.releasePointerCapture(event.pointerId); } catch (_e) {}
});

remoteVideo.addEventListener('wheel', (event) => {
    if (!pc || !inputEnabled) return;
    event.preventDefault();
    sendInput({ type: 'mw', delta: event.deltaY > 0 ? -1 : 1 });
}, { passive: false });

document.addEventListener('keydown', (event) => {
    if (isRemoteExitShortcut(event)) {
        event.preventDefault();
        event.stopPropagation();
        releaseInputCapture();
        return;
    }
    if (!pc || !inputEnabled) return;
    event.preventDefault();
    sendInput({ type: 'kd', code: event.code });
});

document.addEventListener('keyup', (event) => {
    if (isRemoteExitShortcut(event)) {
        event.preventDefault();
        event.stopPropagation();
        return;
    }
    if (!pc || !inputEnabled) return;
    event.preventDefault();
    sendInput({ type: 'ku', code: event.code });
});

copyBtn.addEventListener('click', async () => {
    try {
        await navigator.clipboard.writeText(sessionId);
        log('session id copied');
        setStatus('Session ID copied to clipboard.');
    } catch (_e) {
        log('copy failed');
        setStatus('Copy failed.');
    }
});

openHomeBtn.addEventListener('click', () => {
    redirectToHttpStream();
});
</script>
</body></html>)html";

    // ── Compositor host / session discovery ───────────────────────────────
    // If no compositor session is visible yet, optionally launch one before
    // creating PipeWire/uinput resources. This lets headless setups self-host
    // a Wayland session instead of requiring a manually started desktop.
    CompositorHost compositor_host(cfg.compositor, cfg.capture);
    {
        auto ws = pulsar::server::WaylandSessionInfo{};
        if (cfg.compositor.enabled && !cfg.compositor.command.empty()) {
            if (compositor_host.start()) {
                ws = compositor_host.session_info();
                logger.log(pulsar::core::LogLevel::Info,
                    "  compositor: launched " + cfg.compositor.command);
            }
        }

        if (!ws.found)
            ws = pulsar::server::detect_wayland_session();

        if (ws.found) {
            pulsar::server::apply_wayland_session(ws);
            auto& cap = const_cast<ServerConfig&>(cfg).capture;
            if (cap.dbus_address.empty())    cap.dbus_address    = ws.dbus_address;
            if (cap.xdg_runtime_dir.empty()) cap.xdg_runtime_dir = ws.xdg_runtime_dir;
            logger.log(pulsar::core::LogLevel::Info,
                "  session: wayland seat=" + ws.xdg_seat +
                " wl=" + ws.wayland_display);
        } else {
            logger.log(pulsar::core::LogLevel::Warn,
                "  session: no wayland session detected — "
                "input injection and pipewire capture may not work");
        }
    }

    // ── Shared session state ─────────────────────────────────────────────────
    // The pipeline runs once and the session broker decides which client sink
    // is currently attached, matching Sunshine/xrdp-style reconnect semantics.

    // Server-level PipeWire capture
    std::shared_ptr<pulsar::capture::pipewire::PipeWireCapture> shared_pw_cap;
    if (cfg.capture.backend == "pipewire") {
        shared_pw_cap = std::make_shared<pulsar::capture::pipewire::PipeWireCapture>(
            cfg.capture.dbus_address, cfg.capture.xdg_runtime_dir);
        if (shared_pw_cap->is_open())
            logger.log(pulsar::core::LogLevel::Info,
                "  capture: pipewire (persistent virtual monitor)");
        else {
            logger.log(pulsar::core::LogLevel::Warn,
                "  capture: pipewire init failed — will use synthetic fallback");
            shared_pw_cap.reset();
        }
    }

    if (cfg.compositor.enabled && !cfg.compositor.command.empty() && compositor_host.is_running()) {
        (void)compositor_host.launch_autostart_apps();
    }

    InMemorySessionManager session_mgr;

    // ── Sunshine-style persistent pipeline ───────────────────────────────────
    // The pipeline runs ONCE at server startup (capture→encode always running).
    // SSE clients subscribe/unsubscribe without restarting encode or audio.
    // Result: Stop+Start is near-instantaneous (no NVENC re-init).
    auto broadcast = std::make_shared<SinkBroadcast>();
    const bool persistent_streaming = (cfg.capture.backend == "pipewire") && (shared_pw_cap != nullptr);
    StreamSessionBroker session_broker(
        session_mgr,
        broadcast,
        logger,
        cfg.shared_session.enabled ? cfg.shared_session.max_clients : 1);

    // Persistent pipeline thread: runs for the lifetime of the server.
    // Starts in "suspended" state; resumes when a subscriber connects.
    std::atomic<bool> persistent_stop{false};
    std::thread persistent_pipeline_thread;
    if (persistent_streaming) {
        persistent_pipeline_thread = std::thread([&] {
            while (!persistent_stop.load()) {
                // Use a separate stop flag so pipeline's internal reconnect logic
                // doesn't accidentally set persistent_stop=true via timeout.
                std::atomic<bool> run_stop{false};
                run_pipeline_for_session(cfg, *broadcast, *shared_pw_cap,
                                         logger, run_stop, input, /*persistent=*/true);
                if (!persistent_stop.load()) {
                    logger.log(pulsar::core::LogLevel::Warn,
                        "persistent pipeline: restarting after stop...");
                    std::this_thread::sleep_for(std::chrono::milliseconds(200));
                }
            }
        });
    }

    auto spawn_conn = [&](int fd, std::string req_str) {
        std::thread([fd, req_str=std::move(req_str),
                     &cfg, &logger, &stop, &input,
                     shared_pw_cap, broadcast, persistent_streaming, &session_broker] () mutable {

            auto http_reply_fd = [&](int code, const std::string& ct,
                                     const std::string& body) {
                const std::string status = (code==200)?"200 OK":"404 Not Found";
                std::string resp = "HTTP/1.1 "+status+"\r\n"
                    "Content-Type: "+ct+"\r\n"
                    "Content-Length: "+std::to_string(body.size())+"\r\n"
                    "Access-Control-Allow-Origin: *\r\n"
                    "Connection: close\r\n\r\n"+body;
                ::send(fd, resp.data(), resp.size(), 0);
                ::close(fd);
            };

            const auto path = request_path_only(req_str);

            if (path == "/webrtc") {
                http_reply_fd(200, "text/html; charset=utf-8", kWebRtcPage);
                return;
            }

            if (path == "/") {
                http_reply_fd(200, "text/html; charset=utf-8", kHtmlPage);
                return;
            }

            if (path == "/stream") {
                const std::string sse_hdr =
                    "HTTP/1.1 200 OK\r\n"
                    "Content-Type: text/event-stream\r\n"
                    "Cache-Control: no-cache\r\n"
                    "Access-Control-Allow-Origin: *\r\n"
                    "Connection: keep-alive\r\n\r\n";
                ::send(fd, sse_hdr.data(), sse_hdr.size(), 0);
                logger.log(pulsar::core::LogLevel::Info,
                    "sse: client connected");

                auto sse_transport = std::make_shared<SseTransport>(fd);
                std::string sid = request_query_param(req_str, "sid");
                if (sid.empty()) sid = "default";
                logger.log(pulsar::core::LogLevel::Info,
                    "sse: request path=" + request_path(req_str) + " sid=" + sid +
                    " persistent=" + std::string(persistent_streaming ? "1" : "0"));

                if (persistent_streaming) {
                    // ── Sunshine-style subscribe ──────────────────────────────
                    // Registers with the persistent pipeline. The session broker
                    // keeps reconnects bound to the same sid and only preempts
                    // when a different sid takes over.
                    session_broker.attach(sid, sse_transport);

                    // Heartbeat loop: runs until client disconnects
                    while (!stop.load() && sse_transport->is_alive()) {
                        sse_transport->heartbeat();
                        std::this_thread::sleep_for(std::chrono::milliseconds(100));
                    }

                    // Unsubscribe: session enters suspended state and waits for reconnect
                    session_broker.detach(sid, sse_transport.get());

                } else {
                    // Fallback: per-session pipeline (non-pipewire backends)
                    std::unique_ptr<SyntheticCapture> synth_cap;
                    std::unique_ptr<pulsar::capture::drm_virtual::DrmVirtualCapture> drm_cap;
                    std::unique_ptr<pulsar::capture::xcb::XcbCapture> xcb_cap;
                    std::unique_ptr<pulsar::capture::pipewire::PipeWireCapture> pw_cap;
                    auto& capture = pick_capture(cfg, logger, "sse", synth_cap, drm_cap, xcb_cap, pw_cap, nullptr);

                    std::atomic<bool> session_stop{false};
                    std::thread hb([&] {
                        while (!stop.load() && !session_stop.load()) {
                            sse_transport->heartbeat();
                            if (!sse_transport->is_alive()) break;
                            std::this_thread::sleep_for(std::chrono::milliseconds(100));
                        }
                        session_stop.store(true);
                    });
                    run_pipeline_for_session(cfg, *sse_transport, capture, logger, session_stop, input);
                    session_stop.store(true);
                    hb.join();
                }

                logger.log(pulsar::core::LogLevel::Info, "sse: stream ended");
                return;
            }

            if (path == "/offer") {
                const auto bp = req_str.find("\r\n\r\n");
                std::string browser_offer = (bp!=std::string::npos) ? req_str.substr(bp+4) : "";

                logger.log(pulsar::core::LogLevel::Info,
                    "webrtc: offer received (" + std::to_string(browser_offer.size()) + " bytes)");

                auto transport = std::make_shared<pulsar::transport::webrtc::WebRtcTransport>();
                const std::string answer = transport->handle_offer_and_answer(browser_offer);
                if (answer.empty()) {
                    const std::string e = "HTTP/1.1 500 Error\r\nContent-Length: 0\r\n\r\n";
                    ::send(fd, e.data(), e.size(), 0); ::close(fd); return;
                }
                std::string resp = "HTTP/1.1 200 OK\r\n"
                    "Content-Type: application/sdp\r\n"
                    "Content-Length: "+std::to_string(answer.size())+"\r\n"
                    "Access-Control-Allow-Origin: *\r\nConnection: close\r\n\r\n"+answer;
                ::send(fd, resp.data(), resp.size(), 0); ::close(fd);

                logger.log(pulsar::core::LogLevel::Info, "webrtc: answer sent, waiting for ICE...");
                const auto deadline = std::chrono::steady_clock::now()+std::chrono::seconds(15);
                while (!transport->connected() && !stop.load() &&
                       std::chrono::steady_clock::now() < deadline)
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                if (!transport->connected()) {
                    logger.log(pulsar::core::LogLevel::Warn, "webrtc: DTLS timeout"); return;
                }
                logger.log(pulsar::core::LogLevel::Info, "webrtc: SRTP ready — starting stream");

                std::string sid = request_query_param(req_str, "sid");
                if (sid.empty()) sid = "default";
                logger.log(pulsar::core::LogLevel::Info,
                    "webrtc: request path=" + request_path(req_str) + " sid=" + sid +
                    " persistent=" + std::string(persistent_streaming ? "1" : "0"));

                if (persistent_streaming) {
                    session_broker.attach(sid, transport);
                    while (!stop.load() && transport->connected()) {
                        std::this_thread::sleep_for(std::chrono::milliseconds(100));
                    }
                    session_broker.detach(sid, transport.get());
                    transport->disconnect();
                    logger.log(pulsar::core::LogLevel::Info, "webrtc: session ended");
                } else {
                    wire_transport(*transport, input);
                    std::unique_ptr<SyntheticCapture> synth_cap;
                    std::unique_ptr<pulsar::capture::drm_virtual::DrmVirtualCapture> drm_cap;
                    std::unique_ptr<pulsar::capture::xcb::XcbCapture> xcb_cap;
                    std::unique_ptr<pulsar::capture::pipewire::PipeWireCapture> pw_cap;
                    auto& capture = pick_capture(cfg, logger, "webrtc", synth_cap, drm_cap, xcb_cap, pw_cap, shared_pw_cap);

                    std::atomic<bool> session_stop{false};
                    std::thread sw([&]{ while (!stop.load()&&!session_stop.load())
                        std::this_thread::sleep_for(std::chrono::milliseconds(50));
                        session_stop.store(true); });
                    run_pipeline_for_session(cfg, *transport, capture, logger, session_stop, input);
                    session_stop.store(true); sw.join();
                    transport->disconnect();
                    logger.log(pulsar::core::LogLevel::Info, "webrtc: session ended");
                }
                return;
            }

            // ── CORS preflight ────────────────────────────────────────────────
            if (req_str.starts_with("OPTIONS ")) {
                const std::string ok =
                    "HTTP/1.1 204 No Content\r\n"
                    "Access-Control-Allow-Origin: *\r\n"
                    "Access-Control-Allow-Methods: POST, GET, OPTIONS\r\n"
                    "Access-Control-Allow-Headers: Content-Type\r\n"
                    "Content-Length: 0\r\n\r\n";
                ::send(fd, ok.data(), ok.size(), 0); ::close(fd);
                return;
            }

            // ── Input injection endpoint ──────────────────────────────────────
            // POST /input  body: {"type":"mm","dx":5,"dy":3}
            //              type: mm=mousemove, mb=button, mw=wheel, kd/ku=key
            if (path == "/input") {
                const auto bp = req_str.find("\r\n\r\n");
                std::string sid = request_query_param(req_str, "sid");
                if (sid.empty()) sid = "default";
                session_broker.force_active(sid);
                logger.log(pulsar::core::LogLevel::Debug,
                    "input: request sid=" + sid +
                    " active=" + std::string((!persistent_streaming || session_broker.is_active(sid)) ? "1" : "0"));
                if (bp != std::string::npos) {
                    const std::string body = req_str.substr(bp + 4);
                    try {
                        auto j = nlohmann::json::parse(body);
                        const std::string t = j.value("type", "");
                        pulsar::core::InputEvent ev{};
                        if (t == "mm") {
                            ev.type  = pulsar::core::InputEvent::Type::MouseMove;
                            ev.code  = j.value("dx", 0);
                            ev.value = j.value("dy", 0);
                            input.inject(ev);
                        } else if (t == "ma") {
                            ev.type  = pulsar::core::InputEvent::Type::MouseAbsolute;
                            ev.code  = j.value("x", 0);
                            ev.value = j.value("y", 0);
                            input.inject(ev);
                        } else if (t == "mb") {
                            ev.type  = pulsar::core::InputEvent::Type::MouseButton;
                            ev.code  = j.value("btn", 1);
                            ev.value = j.value("down", 0);
                            input.inject(ev);
                        } else if (t == "mw") {
                            ev.type  = pulsar::core::InputEvent::Type::MouseWheel;
                            ev.value = j.value("delta", 0);
                            input.inject(ev);
                        } else if (t == "kd" || t == "ku") {
                            ev.type  = (t == "kd")
                                ? pulsar::core::InputEvent::Type::KeyDown
                                : pulsar::core::InputEvent::Type::KeyUp;
                            ev.code  = js_code_to_linux(j.value("code", ""));
                            if (ev.code != 0) input.inject(ev);
                        }
                    } catch (...) {}
                }
                const std::string ok =
                    "HTTP/1.1 204 No Content\r\n"
                    "Access-Control-Allow-Origin: *\r\n"
                    "Content-Length: 0\r\n\r\n";
                ::send(fd, ok.data(), ok.size(), 0); ::close(fd);
                return;
            }

            // Unknown path
            const std::string nf = "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\n\r\n";
            ::send(fd, nf.data(), nf.size(), 0); ::close(fd);
        }).detach();  // detach: thread cleans up when it finishes
    };

    while (!stop.load()) {
        fd_set rset; FD_ZERO(&rset); FD_SET(srv_fd, &rset);
        timeval tv{2, 0};
        if (::select(srv_fd + 1, &rset, nullptr, nullptr, &tv) <= 0) continue;

        int client_fd = ::accept(srv_fd, nullptr, nullptr);
        if (client_fd < 0) continue;

        // 3-second receive timeout prevents accept loop from blocking forever
        // if a client connects but never sends HTTP data (e.g., port scanners).
        timeval rtv{3, 0};
        ::setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO,
                     reinterpret_cast<const char*>(&rtv), sizeof(rtv));

        char req_buf[8192] = {};
        ssize_t rn = ::recv(client_fd, req_buf, sizeof(req_buf)-1, 0);
        if (rn <= 0) { ::close(client_fd); continue; }

        spawn_conn(client_fd, std::string(req_buf, rn));
    }
    // Stop persistent pipeline and wait for it to finish
    persistent_stop.store(true);
    if (persistent_pipeline_thread.joinable()) persistent_pipeline_thread.join();
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

    auto shared_input = std::make_shared<pulsar::input::uinput::UinputHandler>();
    if (!shared_input->is_available())
        logger.log(pulsar::core::LogLevel::Warn,
            "  uinput: /dev/uinput not accessible (input injection disabled)");

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
        quic_th = std::thread([&] { run_quic_loop(cfg, logger, g_stop, *shared_input); });
    if (webrtc_enabled)
        webrtc_th = std::thread([&] { run_webrtc_loop(cfg, logger, g_stop, *shared_input); });

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

        run_pipeline_for_session(cfg, transport, capture, logger, session_stop, *shared_input);
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
    if (!cfg) { std::cerr << log_stamp() << "[error] config: " << err << '\n'; return 1; }
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
    auto shared_input = std::make_shared<pulsar::input::uinput::UinputHandler>();
    std::atomic<bool> stop{false};
    std::thread timer([&stop] {
        std::this_thread::sleep_for(std::chrono::seconds(2));
        stop.store(true);
    });

    run_pipeline_for_session(cfg, transport, synth, logger, stop, *shared_input);
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
