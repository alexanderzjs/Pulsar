// transport/vnc/include/transport/vnc/vnc_transport.h
// VNC Transport Adapter — implements ITransport over TCP port 5900.
// Handles the RFB 3.8 handshake (version, security, init) and delivers
// FramebufferUpdate PDUs produced by VncEncoder to a connected VNC client.
// VNC Auth (type 2) authentication is handled internally.
#pragma once

#include "transport.h"
#include <memory>
#include <string>

namespace pulsar::transport::vnc {

// Forward-declare at namespace scope so that free functions in the .cpp
// can use the type without hitting the private-nested-struct restriction.
struct VncTransportImpl;

class VncTransport final : public pulsar::core::ITransport {
public:
    static constexpr int kDefaultPort = 5900;

    VncTransport();
    ~VncTransport() override;

    // Server-side: bind + accept one client.
    bool server_bind(int port = kDefaultPort);
    bool wait_for_client(int timeout_ms = 30000);

    // Set the VNC password used for VNC Auth (type 2).  Must be called before
    // wait_for_client().  If empty, security type "None" (type 1) is used.
    void set_password(const std::string& password);

    // ITransport
    bool connect(const std::string& endpoint) override;
    void disconnect() override;
    void send(pulsar::core::EncodedPacket packet) override;
    void send_batch(std::vector<pulsar::core::EncodedPacket> packets) override;
    void send_audio(pulsar::core::AudioPacket packet) override;
    void send_haptic(const pulsar::core::HapticCommand& cmd) override;
    void send_stats(const pulsar::core::PipelineMetrics& m) override;
    void set_event_callback(std::function<void(pulsar::core::TransportEvent)> cb) override;
    void set_stats_callback(std::function<void(pulsar::core::NetworkStats)> cb) override;
    void set_jitter_buffer(int min_ms, int max_ms) override;
    void set_fec_params(const pulsar::core::FecParams& params) override;
    void set_input_callback(std::function<void(pulsar::core::InputEvent)> cb) override;
    void set_mic_callback(std::function<void(pulsar::core::AudioFrame)> cb) override;

    // IPacketSink
    std::string sink_id() const override { return "vnc"; }
    void on_packet(pulsar::core::EncodedPacket p) override { send(std::move(p)); }
    void on_audio(pulsar::core::AudioPacket p) override   { send_audio(std::move(p)); }

private:
    std::unique_ptr<VncTransportImpl> impl_;
};

} // namespace pulsar::transport::vnc
