// server/include/session_services.h
// Stage 3 service class declarations:
//   OutputMultiplexer, FileRecorder, InputArbiter, WakeOnLanServer, AppManager

#pragma once

#include "config.h"
#include "app_manager.h"
#include "multiplexer.h"
#include "recorder.h"
#include "shared_session.h"
#include "session_detect.h"
#include "transport.h"
#include "wake_on_lan.h"

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace pulsar::server {

// ─── OutputMultiplexer ────────────────────────────────────────────────────────
// Implements ITransport: wraps a primary transport, fans out packets to sinks.
// Passes to run_pipeline() transparently; no pipeline changes needed.
class OutputMultiplexer final : public pulsar::core::ITransport {
public:
    explicit OutputMultiplexer(pulsar::core::ITransport& primary);

    void add_sink(std::shared_ptr<pulsar::core::IPacketSink> sink);
    void remove_sink(const std::string& sink_id);

    // ITransport: packet fanout
    void send(pulsar::core::EncodedPacket packet) override;
    void send_batch(std::vector<pulsar::core::EncodedPacket> packets) override;
    void send_audio(pulsar::core::AudioPacket packet) override;

    // ITransport: delegate to primary
    bool connect(const std::string& endpoint) override;
    void disconnect() override;
    void set_event_callback(std::function<void(pulsar::core::TransportEvent)> cb) override;
    void set_stats_callback(std::function<void(pulsar::core::NetworkStats)> cb) override;
    void set_input_callback(std::function<void(pulsar::core::InputEvent)> cb) override;
    void set_mic_callback(std::function<void(pulsar::core::AudioFrame)> cb) override;
    void set_jitter_buffer(int min_ms, int max_ms) override;
    void set_fec_params(const pulsar::core::FecParams& params) override;
    void send_haptic(const pulsar::core::HapticCommand& cmd) override;
    void send_stats(const pulsar::core::PipelineMetrics& m) override;

    // IPacketSink
    std::string sink_id() const override;
    void on_packet(pulsar::core::EncodedPacket packet) override;
    void on_audio(pulsar::core::AudioPacket packet) override;

private:
    pulsar::core::ITransport& primary_;
    std::vector<std::shared_ptr<pulsar::core::IPacketSink>> extra_;
    mutable std::mutex mu_;
};

// ─── FileRecorder ─────────────────────────────────────────────────────────────
// Writes raw Annex-B H.264/H.265 to a file. Implements IRecorder (: IPacketSink).
class FileRecorder final : public pulsar::core::IRecorder {
public:
    ~FileRecorder();
    void start(const std::string& output_path) override;
    void stop() override;
    std::string sink_id() const override;
    void on_packet(pulsar::core::EncodedPacket packet) override;
    void on_audio(pulsar::core::AudioPacket packet) override;
private:
    FILE* fp_ = nullptr;
    mutable std::mutex mu_;
};

// ─── InputArbiter ─────────────────────────────────────────────────────────────
// Routes input events by device type; no binding = allow all (full control).
class InputArbiter final : public pulsar::core::IInputArbiter {
public:
    void bind(const pulsar::core::ClientDeviceBinding& binding) override;
    void unbind(const std::string& client_id) override;
    bool allow(const std::string& client_id,
               const pulsar::core::InputEvent& event) const override;
    std::vector<pulsar::core::ClientDeviceBinding> list_bindings() const override;
private:
    std::vector<pulsar::core::ClientDeviceBinding> bindings_;
    mutable std::mutex mu_;
};

// ─── WakeOnLanServer ──────────────────────────────────────────────────────────
class WakeOnLanServer final : public pulsar::core::IWakeOnLan {
public:
    ~WakeOnLanServer();
    void listen(int port = 9) override;
    void stop() override;
    void send(const std::string& mac_address,
              const std::string& broadcast_addr = "255.255.255.255") override;
private:
    int  fd_ = -1;
    std::atomic<bool> running_{false};
    std::thread thread_;
};

// ─── CompositorHost ──────────────────────────────────────────────────────────
// Starts an external Wayland compositor process and tracks its session env.
class CompositorHost final {
public:
    CompositorHost(const CompositorConfig& compositor, const CaptureConfig& capture);
    ~CompositorHost();

    bool start();
    void stop();
    bool is_running() const;
    pulsar::server::WaylandSessionInfo session_info() const;
    bool launch_autostart_apps();

private:
    bool wait_for_socket() const;
    bool wait_for_path(const std::string& path) const;
    bool launch_session_bus();
    bool launch_pipewire_stack();
    bool launch_compositor();
    pid_t spawn_process(const std::string& label,
                        const std::vector<std::string>& argv,
                        const std::vector<std::pair<std::string, std::string>>& env) const;
    void kill_process(pid_t pid, const std::string& label);

    CompositorConfig config_;
    CaptureConfig capture_;
    pid_t pid_ = -1;
    pid_t dbus_pid_ = -1;
    pid_t pipewire_pid_ = -1;
    pid_t wireplumber_pid_ = -1;
    pid_t pipewire_pulse_pid_ = -1;
    std::string dbus_address_;
    std::string runtime_dir_;
    std::string socket_name_;
};

// ─── AppManager ───────────────────────────────────────────────────────────────
class AppManager final : public pulsar::core::IAppManager {
public:
    void set_apps(std::vector<AppEntry> apps);
    std::vector<pulsar::core::AppEntry> list_apps() const override;
    bool launch(const std::string& app_id) override;
    bool terminate(const std::string& app_id) override;
    bool is_running(const std::string& app_id) const override;
private:
    std::vector<AppEntry> apps_;
    std::unordered_map<std::string, pid_t> running_;
    mutable std::mutex mu_;
};

} // namespace pulsar::server
