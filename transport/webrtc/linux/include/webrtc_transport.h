#pragma once

#include "transport.h"
#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace pulsar::transport::webrtc {

// WebRTC transport using libnice for ICE (NAT traversal).
// Media is sent as SRTP over the ICE channel (video/audio).
// Input events are received over the DataChannel (SCTP-over-DTLS), which
// is how real browser clients send keyboard/mouse events.
//
// Phase 2 MVP:
//   - ICE candidate gathering via libnice
//   - SRTP-like framing (placeholder for DTLS-SRTP key exchange)
//   - DataChannel input decoding (text JSON messages)
//
// Phase 3 target: full DTLS-SRTP handshake + SDP offer/answer exchange.
class WebRtcTransport final : public pulsar::core::ITransport {
public:
    WebRtcTransport();
    ~WebRtcTransport() override;

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

    // Generate and return an SDP offer (sent to the client for ICE negotiation).
    std::string generate_sdp_offer();
    // Process an SDP answer received from the client.
    bool apply_sdp_answer(const std::string& sdp);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;

    std::function<void(pulsar::core::NetworkStats)>   stats_cb_;
    std::function<void(pulsar::core::AudioFrame)>     mic_cb_;

    pulsar::core::FecParams fec_params_{};
    mutable std::mutex      send_mtx_;

public:  // accessed by static GLib callbacks
    std::atomic<bool>       connected_{false};
    std::function<void(pulsar::core::TransportEvent)> event_cb_;
    std::function<void(pulsar::core::InputEvent)>     input_cb_;
};

} // namespace pulsar::transport::webrtc
