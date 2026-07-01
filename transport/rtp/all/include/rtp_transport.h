#pragma once

#include "transport.h"
#include <cstdint>
#include <mutex>
#include <netinet/in.h>
#include <vector>

namespace pulsar::transport::rtp {

// RTP transport over UDP — standard RFC 3550 framing.
//
// Video:  UDP port N       (default 47984)  payload type 96 = H.264 / 97 = H.265
// Audio:  UDP port N+2     (default 47986)  payload type 111 = Opus
// RTCP:   UDP port N+1 / N+3               (not yet implemented)
//
// Clock rates:
//   Video:  90 000 Hz   (standard for RTP video)
//   Audio:  48 000 Hz   (Opus native sample rate)
//
// H.264 packetization follows RFC 6184:
//   Single NAL unit packets for NAL ≤ kMTU bytes
//   FU-A fragmentation for larger NAL units
//
// Usage (server push):
//   RtpTransport t;
//   t.connect("host:47984");          // set client video/audio addresses
//   t.send(encoded_packet);           // → UDP RTP datagrams to client
//   t.send_audio(audio_packet);       // → UDP RTP datagrams to client
//
// Usage (server accept):
//   RtpTransport t;
//   t.server_bind(47984);             // bind UDP sockets
//   t.wait_for_client(5000);          // first UDP datagram = client address
//   t.send(…);                        // push RTP to discovered client

class RtpTransport final : public pulsar::core::ITransport {
public:
    static constexpr int    kDefaultVideoPort = 47984;
    static constexpr int    kDefaultAudioPort = 47986;
    static constexpr uint8_t kPtH264          = 96;
    static constexpr uint8_t kPtH265          = 97;
    static constexpr uint8_t kPtOpus          = 111;
    static constexpr size_t  kMTU             = 1200; // safe UDP payload size

    RtpTransport();
    ~RtpTransport() override;

    // ITransport -----------------------------------------------------------
    bool connect(const std::string& endpoint) override; // "host:port" → push to host
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

    // IPacketSink ----------------------------------------------------------
    std::string sink_id() const override;
    void on_packet(pulsar::core::EncodedPacket packet) override;
    void on_audio(pulsar::core::AudioPacket packet) override;

    // Stats ----------------------------------------------------------------
    bool   connected() const;
    size_t video_packets_sent() const;
    size_t audio_packets_sent() const;

    // Server-mode helpers --------------------------------------------------
    // Bind UDP sockets on video_port (and video_port+2 for audio).
    bool server_bind(int video_port = kDefaultVideoPort);
    // Block up to timeout_ms waiting for any UDP datagram from a client.
    // Records client address so RTP can be pushed to them.
    bool wait_for_client(int timeout_ms = 10000);
    // Convenience: bind + wait in one call.
    bool server_accept(int video_port = kDefaultVideoPort, int timeout_ms = 10000);
    // accept_from / set_fd kept for factory compatibility (remapped to server_bind).
    bool accept_from(int /*unused*/) { return server_bind(); }
    void set_fd(int /*unused*/) {}
    // Wait for the NEXT client (reconnect within same session).
    bool reconnect_accept(int /*unused*/, int timeout_ms = 500) {
        return wait_for_client(timeout_ms);
    }

    // Print an SDP description to stdout (pipe to file for ffplay).
    void print_sdp(const std::string& server_ip = "127.0.0.1") const;

private:
    // ── RTP send helpers ──────────────────────────────────────────────────
    bool udp_send(int fd, const sockaddr_in& dest, const void* data, size_t size);

    // Packetise an H.264/H.265 Annex-B buffer as RTP.
    // Emits one or more UDP datagrams (single NAL or FU-A).
    void send_rtp_video(const uint8_t* data, size_t size,
                        int64_t pts_us, bool is_keyframe,
                        pulsar::core::CodecType codec);

    // Wrap an Opus frame in a single RTP packet.
    void send_rtp_audio(const uint8_t* data, size_t size, int64_t pts_us);

    // Build a 12-byte RTP header into `out`.
    static void build_rtp_header(uint8_t* out,
                                 bool marker, uint8_t pt,
                                 uint16_t seq, uint32_t ts, uint32_t ssrc);

    // ── FEC ───────────────────────────────────────────────────────────────
    void fec_push_udp(int fd, const sockaddr_in& dest,
                      const uint8_t* data, size_t size);
    void fec_flush_parity(int fd, const sockaddr_in& dest);

    // ── State ─────────────────────────────────────────────────────────────
    int video_fd_ = -1;  // UDP socket for video
    int audio_fd_ = -1;  // UDP socket for audio

    sockaddr_in client_video_{};
    sockaddr_in client_audio_{};
    bool client_known_ = false;

    // RTP sequence counters and SSRCs
    uint32_t video_ssrc_ = 0;
    uint16_t video_seq_  = 0;
    uint32_t audio_ssrc_ = 0;
    uint16_t audio_seq_  = 0;

    size_t video_packets_sent_ = 0;
    size_t audio_packets_sent_ = 0;

    mutable std::mutex send_mtx_;

    // FEC
    pulsar::core::FecParams    fec_params_{};
    std::vector<std::vector<uint8_t>> fec_window_;
    std::vector<uint8_t>              fec_parity_;

    // Callbacks
    std::function<void(pulsar::core::TransportEvent)> event_cb_;
    std::function<void(pulsar::core::NetworkStats)>   stats_cb_;
    std::function<void(pulsar::core::InputEvent)>     input_cb_;
    std::function<void(pulsar::core::AudioFrame)>     mic_cb_;
};

} // namespace pulsar::transport::rtp
