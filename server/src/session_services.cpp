// server/src/session_services.cpp
// Stage 3 service implementations:
//   OutputMultiplexer — wraps primary ITransport, fans out packets to extra sinks
//   FileRecorder      — raw Annex-B video file sink (IRecorder / IPacketSink)
//   InputArbiter      — routes input events by device type (IInputArbiter)
//   WakeOnLanServer   — UDP Magic Packet listener + sender (IWakeOnLan)
//   AppManager        — fork/exec game launcher (IAppManager)

#include "session_services.h"

#include <algorithm>
#include <arpa/inet.h>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <mutex>
#include <netinet/in.h>
#include <signal.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

namespace pulsar::server {

// ─── OutputMultiplexer ────────────────────────────────────────────────────────
// Wraps a primary ITransport; fans out send()/send_audio() to all sinks.
// All ITransport control methods (events, FEC, input) delegate to primary.

OutputMultiplexer::OutputMultiplexer(pulsar::core::ITransport& primary)
    : primary_(primary) {}

void OutputMultiplexer::add_sink(std::shared_ptr<pulsar::core::IPacketSink> sink) {
    std::lock_guard lk(mu_);
    extra_.push_back(std::move(sink));
}
void OutputMultiplexer::remove_sink(const std::string& id) {
    std::lock_guard lk(mu_);
    extra_.erase(std::remove_if(extra_.begin(), extra_.end(),
        [&](const auto& s) { return s->sink_id() == id; }), extra_.end());
}

void OutputMultiplexer::send(pulsar::core::EncodedPacket pkt) {
    primary_.send(pkt);
    std::lock_guard lk(mu_);
    for (auto& s : extra_) s->on_packet(pkt);
}
void OutputMultiplexer::send_batch(std::vector<pulsar::core::EncodedPacket> pkts) {
    for (auto& p : pkts) send(p);
}
void OutputMultiplexer::send_audio(pulsar::core::AudioPacket pkt) {
    primary_.send_audio(pkt);
    std::lock_guard lk(mu_);
    for (auto& s : extra_) s->on_audio(pkt);
}

// Delegate everything else to primary
bool OutputMultiplexer::connect(const std::string& ep)       { return primary_.connect(ep); }
void OutputMultiplexer::disconnect()                          { primary_.disconnect(); }
void OutputMultiplexer::set_event_callback(std::function<void(pulsar::core::TransportEvent)> cb)  { primary_.set_event_callback(std::move(cb)); }
void OutputMultiplexer::set_stats_callback(std::function<void(pulsar::core::NetworkStats)> cb)    { primary_.set_stats_callback(std::move(cb)); }
void OutputMultiplexer::set_input_callback(std::function<void(pulsar::core::InputEvent)> cb)      { primary_.set_input_callback(std::move(cb)); }
void OutputMultiplexer::set_mic_callback(std::function<void(pulsar::core::AudioFrame)> cb)        { primary_.set_mic_callback(std::move(cb)); }
void OutputMultiplexer::set_jitter_buffer(int mn, int mx)    { primary_.set_jitter_buffer(mn, mx); }
void OutputMultiplexer::set_fec_params(const pulsar::core::FecParams& p)  { primary_.set_fec_params(p); }
void OutputMultiplexer::send_haptic(const pulsar::core::HapticCommand& c) { primary_.send_haptic(c); }
void OutputMultiplexer::send_stats(const pulsar::core::PipelineMetrics& m){ primary_.send_stats(m); }
std::string OutputMultiplexer::sink_id() const { return "multiplexer"; }
void OutputMultiplexer::on_packet(pulsar::core::EncodedPacket p) { send(std::move(p)); }
void OutputMultiplexer::on_audio(pulsar::core::AudioPacket p)    { send_audio(std::move(p)); }

// ─── FileRecorder ─────────────────────────────────────────────────────────────
// Writes raw Annex-B H.264/H.265 video to a .h264/.h265 file.
// MP4 muxing is a future enhancement.

FileRecorder::~FileRecorder() { stop(); }

void FileRecorder::start(const std::string& path) {
    std::lock_guard lk(mu_);
    fp_ = ::fopen(path.c_str(), "wb");
    if (!fp_) std::cerr << "[recorder] cannot open: " << path << "\n";
    else      std::cerr << "[recorder] recording to: " << path << "\n";
}
void FileRecorder::stop() {
    std::lock_guard lk(mu_);
    if (fp_) { ::fclose(fp_); fp_ = nullptr; }
}
std::string FileRecorder::sink_id() const { return "recorder"; }
void FileRecorder::on_packet(pulsar::core::EncodedPacket pkt) {
    if (!pkt.buffer || !fp_) return;
    std::lock_guard lk(mu_);
    static const uint8_t sc[4] = {0, 0, 0, 1};  // Annex-B start code
    ::fwrite(sc, 1, 4, fp_);
    ::fwrite(pkt.buffer->data(), 1, pkt.buffer->size(), fp_);
}
void FileRecorder::on_audio(pulsar::core::AudioPacket) {} // future: mux audio

// ─── InputArbiter ─────────────────────────────────────────────────────────────
// FreeForAll + device-type routing:
//   - No binding registered for client_id → allow all events (default: full control)
//   - Binding registered → only allow events whose type is in claimed_types

void InputArbiter::bind(const pulsar::core::ClientDeviceBinding& b) {
    std::lock_guard lk(mu_);
    for (auto& x : bindings_)
        if (x.client_id == b.client_id) { x = b; return; }
    bindings_.push_back(b);
}
void InputArbiter::unbind(const std::string& id) {
    std::lock_guard lk(mu_);
    bindings_.erase(std::remove_if(bindings_.begin(), bindings_.end(),
        [&](const auto& x) { return x.client_id == id; }), bindings_.end());
}
bool InputArbiter::allow(const std::string& id,
                          const pulsar::core::InputEvent& ev) const {
    std::lock_guard lk(mu_);
    for (const auto& b : bindings_) {
        if (b.client_id != id) continue;
        return std::find(b.claimed_types.begin(), b.claimed_types.end(), ev.type)
               != b.claimed_types.end();
    }
    return true; // no binding = allow everything
}
std::vector<pulsar::core::ClientDeviceBinding> InputArbiter::list_bindings() const {
    std::lock_guard lk(mu_);
    return bindings_;
}

// ─── WakeOnLanServer ──────────────────────────────────────────────────────────

WakeOnLanServer::~WakeOnLanServer() { stop(); }

void WakeOnLanServer::listen(int port) {
    fd_ = ::socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (fd_ < 0) { std::cerr << "[wol] socket failed\n"; return; }
    int opt = 1;
    ::setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    sockaddr_in addr{}; addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port        = htons(static_cast<uint16_t>(port));
    ::bind(fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));

    running_.store(true);
    thread_ = std::thread([this] {
        uint8_t buf[256];
        while (running_.load()) {
            fd_set rset; FD_ZERO(&rset); FD_SET(fd_, &rset);
            timeval tv{1, 0};
            if (::select(fd_ + 1, &rset, nullptr, nullptr, &tv) > 0) {
                ssize_t n = ::recv(fd_, buf, sizeof(buf), 0);
                if (n == 102 && buf[0] == 0xFF && buf[1] == 0xFF)
                    std::cerr << "[wol] Magic Packet received — system will wake\n";
            }
        }
    });
    std::cerr << "[wol] listening on UDP port " << port << "\n";
}

void WakeOnLanServer::stop() {
    running_.store(false);
    if (thread_.joinable()) thread_.join();
    if (fd_ >= 0) { ::close(fd_); fd_ = -1; }
}

void WakeOnLanServer::send(const std::string& mac,
                            const std::string& broadcast_addr) {
    uint8_t mac_bytes[6] = {};
    if (::sscanf(mac.c_str(), "%hhx:%hhx:%hhx:%hhx:%hhx:%hhx",
            &mac_bytes[0], &mac_bytes[1], &mac_bytes[2],
            &mac_bytes[3], &mac_bytes[4], &mac_bytes[5]) != 6) {
        std::cerr << "[wol] invalid MAC: " << mac << "\n";
        return;
    }
    uint8_t pkt[102];
    std::memset(pkt, 0xFF, 6);
    for (int i = 0; i < 16; ++i) std::memcpy(pkt + 6 + i * 6, mac_bytes, 6);

    int s = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0) return;
    int opt = 1;
    ::setsockopt(s, SOL_SOCKET, SO_BROADCAST, &opt, sizeof(opt));
    sockaddr_in addr{}; addr.sin_family = AF_INET;
    addr.sin_port = htons(9);
    ::inet_pton(AF_INET, broadcast_addr.c_str(), &addr.sin_addr);
    ::sendto(s, pkt, sizeof(pkt), 0,
             reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    ::close(s);
    std::cerr << "[wol] Magic Packet sent to " << mac << "\n";
}

// ─── AppManager ───────────────────────────────────────────────────────────────
// Launches game/app processes via /bin/sh -c.
// Tracks PIDs; is_running() uses WNOHANG waitpid.

void AppManager::set_apps(std::vector<AppEntry> apps) {
    apps_ = std::move(apps);
}

std::vector<pulsar::core::AppEntry> AppManager::list_apps() const {
    std::vector<pulsar::core::AppEntry> out;
    for (const auto& a : apps_)
        out.push_back({a.id, a.name, a.executable, a.args, a.working_dir, {}});
    return out;
}

bool AppManager::launch(const std::string& app_id) {
    for (const auto& e : apps_) {
        if (e.id != app_id) continue;
        pid_t pid = ::fork();
        if (pid == 0) {
            // Child: optionally chdir, then exec via shell
            if (!e.working_dir.empty()) {
                int rc = ::chdir(e.working_dir.c_str());
                (void)rc; // best-effort
            }
            const std::string cmd = e.args.empty()
                ? e.executable
                : e.executable + " " + e.args;
            ::execl("/bin/sh", "sh", "-c", cmd.c_str(), nullptr);
            ::_exit(1);
        }
        if (pid > 0) {
            std::lock_guard lk(mu_);
            running_[app_id] = pid;
            std::cerr << "[app] launched '" << e.name << "' pid=" << pid << "\n";
            return true;
        }
        return false;
    }
    std::cerr << "[app] unknown app_id: " << app_id << "\n";
    return false;
}

bool AppManager::terminate(const std::string& app_id) {
    std::lock_guard lk(mu_);
    auto it = running_.find(app_id);
    if (it == running_.end()) return false;
    ::kill(it->second, SIGTERM);
    running_.erase(it);
    return true;
}

bool AppManager::is_running(const std::string& app_id) const {
    std::lock_guard lk(mu_);
    auto it = running_.find(app_id);
    if (it == running_.end()) return false;
    return ::waitpid(it->second, nullptr, WNOHANG) == 0;
}

} // namespace pulsar::server
