// transport/rdp/include/transport/rdp/rdp_transport.h
// RDP Transport Adapter — implements ITransport over TCP port 3389.
// Handles the X.224 / MCS / T.128 handshake and sends Bitmap Update PDUs
// produced by RdpEncoder to a connected mstsc (or compatible) client.
// NLA/CredSSP authentication is handled internally; IAuthProvider is NOT used.
#pragma once

#include "transport.h"
#include <memory>
#include <string>

namespace pulsar::transport::rdp {

// Forward-declare at namespace scope so that free functions in the .cpp
// can use the type without hitting the private-nested-struct restriction.
struct RdpTransportImpl;

class RdpTransport final : public pulsar::core::ITransport {
public:
    static constexpr int kDefaultPort = 3389;

    RdpTransport();
    ~RdpTransport() override;

    // Server-side: bind + accept one client.
    bool server_bind(int port = kDefaultPort);
    bool wait_for_client(int timeout_ms = 30000);

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
    std::string sink_id() const override { return "rdp"; }
    void on_packet(pulsar::core::EncodedPacket p) override { send(std::move(p)); }
    void on_audio(pulsar::core::AudioPacket p) override   { send_audio(std::move(p)); }

private:
    std::unique_ptr<RdpTransportImpl> impl_;
};

} // namespace pulsar::transport::rdp
