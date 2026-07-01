#pragma once

#include "transport.h"
#include <atomic>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace pulsar::transport::quic {

// QUIC transport using libngtcp2.
// Provides low-latency UDP-based streaming with:
//   - Built-in FEC (via set_fec_params)
//   - Congestion control feedback → NetworkStats
//   - Stream 0: video,  Stream 1: audio,  Stream 2: input DataChannel
//
// Phase 2 MVP: UDP socket + QUIC framing; TLS 1.3 handshake is wired via
// a minimal self-signed certificate.  Full browser interoperability requires
// adding DTLS-SRTP and SCTP (Phase 3).
class QuicTransport final : public pulsar::core::ITransport {
public:
    QuicTransport();
    ~QuicTransport() override;

    bool connect(const std::string& endpoint) override;
    void disconnect() override;
    void send(pulsar::core::EncodedPacket packet) override;
    void send_batch(std::vector<pulsar::core::EncodedPacket> packets) override;
    void set_event_callback(std::function<void(pulsar::core::TransportEvent)> cb) override;
    void set_stats_callback(std::function<void(pulsar::core::NetworkStats)> cb) override;
    void set_jitter_buffer(int min_ms, int max_ms) override;
    void set_fec_params(const pulsar::core::FecParams& params) override;
    void set_input_callback(std::function<void(pulsar::core::InputEvent)> cb) override;
    void send_audio(pulsar::core::AudioPacket packet) override;
    void send_haptic(const pulsar::core::HapticCommand& cmd) override;
    void send_stats(const pulsar::core::PipelineMetrics& m) override;
    void set_mic_callback(std::function<void(pulsar::core::AudioFrame)> cb) override;

    // IPacketSink
    std::string sink_id() const override;
    void on_packet(pulsar::core::EncodedPacket packet) override;
    void on_audio(pulsar::core::AudioPacket packet) override;

    bool connected() const;

    // Server mode: bind UDP port and wait for the first client QUIC Initial.
    bool server_accept(int port, int timeout_ms = 10000);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;

    std::function<void(pulsar::core::TransportEvent)> event_cb_;
    std::function<void(pulsar::core::NetworkStats)>   stats_cb_;
    std::function<void(pulsar::core::InputEvent)>     input_cb_;
    std::function<void(pulsar::core::AudioFrame)>     mic_cb_;

    pulsar::core::FecParams fec_params_{};
    mutable std::mutex      send_mtx_;
    std::atomic<bool>       connected_{false};
};

} // namespace pulsar::transport::quic
