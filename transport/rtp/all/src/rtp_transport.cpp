// transport/rtp/src/rtp_transport.cpp
// Standard RTP/UDP transport (RFC 3550, RFC 6184).

#include "rtp_transport.h"

#include <arpa/inet.h>
#include <cstring>
#include <cstdlib>
#include <ctime>
#include <iostream>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <unistd.h>

namespace pulsar::transport::rtp {

// ─── RTP Header ──────────────────────────────────────────────────────────────
//  0                   1                   2                   3
//  0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
// +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
// |V=2|P|X|  CC   |M|     PT      |       sequence number         |
// +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
// |                           timestamp                           |
// +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
// |           synchronization source (SSRC) identifier           |
// +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
static constexpr int kRtpHeaderSize = 12;

void RtpTransport::build_rtp_header(uint8_t* out,
                                    bool marker, uint8_t pt,
                                    uint16_t seq, uint32_t ts, uint32_t ssrc)
{
    out[0]  = 0x80;                             // V=2, P=0, X=0, CC=0
    out[1]  = static_cast<uint8_t>((marker ? 0x80 : 0) | (pt & 0x7F));
    out[2]  = static_cast<uint8_t>(seq >> 8);
    out[3]  = static_cast<uint8_t>(seq & 0xFF);
    out[4]  = static_cast<uint8_t>(ts >> 24);
    out[5]  = static_cast<uint8_t>(ts >> 16);
    out[6]  = static_cast<uint8_t>(ts >> 8);
    out[7]  = static_cast<uint8_t>(ts & 0xFF);
    out[8]  = static_cast<uint8_t>(ssrc >> 24);
    out[9]  = static_cast<uint8_t>(ssrc >> 16);
    out[10] = static_cast<uint8_t>(ssrc >> 8);
    out[11] = static_cast<uint8_t>(ssrc & 0xFF);
}

// ─── Construction ─────────────────────────────────────────────────────────────
RtpTransport::RtpTransport() {
    // Random SSRCs
    std::srand(static_cast<unsigned>(std::time(nullptr)));
    video_ssrc_ = static_cast<uint32_t>(std::rand());
    audio_ssrc_ = static_cast<uint32_t>(std::rand()) ^ 0xFFFF0000u;
}

RtpTransport::~RtpTransport() { disconnect(); }

// ─── UDP helpers ──────────────────────────────────────────────────────────────
static int create_udp_socket() {
    int fd = ::socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return -1;
    // SO_SNDBUF: increase to handle burst video frames
    int sndbuf = 4 * 1024 * 1024;
    ::setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf));
    return fd;
}

bool RtpTransport::udp_send(int fd, const sockaddr_in& dest,
                            const void* data, size_t size)
{
    if (fd < 0) return false;
    return ::sendto(fd, data, size, 0,
                    reinterpret_cast<const sockaddr*>(&dest), sizeof(dest)) > 0;
}

static void make_addr(sockaddr_in& out, const std::string& host, int port) {
    std::memset(&out, 0, sizeof(out));
    out.sin_family = AF_INET;
    out.sin_port   = htons(static_cast<uint16_t>(port));
    ::inet_pton(AF_INET, host.c_str(), &out.sin_addr);
}

static std::pair<std::string,int> parse_ep(const std::string& ep) {
    const auto s = ep.find("://");
    const std::string hp = (s == std::string::npos) ? ep : ep.substr(s + 3);
    const auto c = hp.rfind(':');
    if (c == std::string::npos) return {hp, RtpTransport::kDefaultVideoPort};
    return {hp.substr(0, c), std::stoi(hp.substr(c + 1))};
}

// ─── connect (client push mode) ───────────────────────────────────────────────
bool RtpTransport::connect(const std::string& endpoint) {
    disconnect();
    auto [host, port] = parse_ep(endpoint);
    make_addr(client_video_, host, port);
    make_addr(client_audio_, host, port + 2);

    video_fd_ = create_udp_socket();
    audio_fd_ = create_udp_socket();
    if (video_fd_ < 0 || audio_fd_ < 0) { disconnect(); return false; }

    client_known_ = true;
    if (event_cb_) event_cb_(pulsar::core::TransportEvent::Ready);
    return true;
}

// ─── server_bind + wait_for_client ────────────────────────────────────────────
bool RtpTransport::server_bind(int video_port) {
    disconnect();

    video_fd_ = create_udp_socket();
    audio_fd_ = create_udp_socket();
    if (video_fd_ < 0 || audio_fd_ < 0) { disconnect(); return false; }

    // Bind video socket so we can receive the client's "hello" datagram.
    sockaddr_in vaddr{};
    vaddr.sin_family      = AF_INET;
    vaddr.sin_addr.s_addr = INADDR_ANY;
    vaddr.sin_port        = htons(static_cast<uint16_t>(video_port));
    if (::bind(video_fd_, reinterpret_cast<sockaddr*>(&vaddr), sizeof(vaddr)) != 0) {
        disconnect(); return false;
    }
    return true;
}

bool RtpTransport::wait_for_client(int timeout_ms) {
    if (video_fd_ < 0) return false;
    fd_set rset; FD_ZERO(&rset); FD_SET(video_fd_, &rset);
    timeval tv{ .tv_sec = timeout_ms / 1000, .tv_usec = (timeout_ms % 1000) * 1000 };
    if (::select(video_fd_ + 1, &rset, nullptr, nullptr, &tv) <= 0) return false;

    // Read the join datagram to learn client address.
    uint8_t buf[32]{};
    sockaddr_in peer{}; socklen_t plen = sizeof(peer);
    ::recvfrom(video_fd_, buf, sizeof(buf), 0,
               reinterpret_cast<sockaddr*>(&peer), &plen);

    client_video_ = peer;
    // Always push to the client's well-known RTP ports, NOT the ephemeral
    // source port of the hello datagram.  The client must have these ports
    // open for receiving (e.g. ffplay rtp://0.0.0.0:47984).
    client_video_.sin_port = htons(kDefaultVideoPort);
    client_audio_ = peer;
    client_audio_.sin_port = htons(kDefaultAudioPort);

    // Repoint audio_fd_ → client_audio_
    if (audio_fd_ < 0) audio_fd_ = create_udp_socket();

    char client_ip[INET_ADDRSTRLEN]{};
    ::inet_ntop(AF_INET, &peer.sin_addr, client_ip, sizeof(client_ip));
    std::cerr << "[rtp] client registered: " << client_ip
              << " video→" << ntohs(client_video_.sin_port)
              << " audio→" << ntohs(client_audio_.sin_port) << "\n";

    client_known_ = true;
    if (event_cb_) event_cb_(pulsar::core::TransportEvent::Ready);
    return true;
}

bool RtpTransport::server_accept(int video_port, int timeout_ms) {
    return server_bind(video_port) && wait_for_client(timeout_ms);
}

// ─── disconnect ───────────────────────────────────────────────────────────────
void RtpTransport::disconnect() {
    client_known_ = false;
    if (video_fd_ >= 0) { ::close(video_fd_); video_fd_ = -1; }
    if (audio_fd_ >= 0) { ::close(audio_fd_); audio_fd_ = -1; }
    if (event_cb_) event_cb_(pulsar::core::TransportEvent::Disconnected);
}

// ─── H.264 RTP packetization (RFC 6184) ──────────────────────────────────────
// Returns pointer to NAL data and size after stripping Annex-B start code.
static const uint8_t* strip_start_code(const uint8_t* p, size_t& remaining) {
    if (remaining >= 4 && p[0]==0 && p[1]==0 && p[2]==0 && p[3]==1) {
        p += 4; remaining -= 4;
    } else if (remaining >= 3 && p[0]==0 && p[1]==0 && p[2]==1) {
        p += 3; remaining -= 3;
    }
    return p;
}

void RtpTransport::send_rtp_video(const uint8_t* buf, size_t total_size,
                                   int64_t pts_us, bool is_keyframe,
                                   pulsar::core::CodecType codec)
{
    if (!client_known_ || video_fd_ < 0) return;

    const uint8_t pt = (codec == pulsar::core::CodecType::H265) ? kPtH265 : kPtH264;
    // RTP timestamp: 90 kHz clock
    const uint32_t rtp_ts = static_cast<uint32_t>(pts_us * 9 / 100);

    // Walk the Annex-B buffer and send each NAL unit.
    const uint8_t* pos  = buf;
    size_t         left = total_size;

    // Find NAL unit boundaries (start codes 00 00 00 01 or 00 00 01)
    while (left > 0) {
        // Skip leading start code
        if (left >= 4 && pos[0]==0 && pos[1]==0 && pos[2]==0 && pos[3]==1)
            { pos += 4; left -= 4; }
        else if (left >= 3 && pos[0]==0 && pos[1]==0 && pos[2]==1)
            { pos += 3; left -= 3; }
        else if (left == 0) break;

        // Find end of this NAL unit (next start code or end of buffer)
        size_t nal_size = left;
        for (size_t i = 0; i + 3 <= left; ++i) {
            if ((pos[i]==0 && pos[i+1]==0 && pos[i+2]==0 && i+3<left && pos[i+3]==1) ||
                (pos[i]==0 && pos[i+1]==0 && pos[i+2]==1)) {
                nal_size = i;
                break;
            }
        }
        if (nal_size == 0) { pos += left; left = 0; break; }

        const bool last_nal = (nal_size >= left);
        const bool marker   = last_nal; // M bit set on last NAL of access unit

        if (nal_size <= kMTU) {
            // ── Single NAL unit packet ────────────────────────────────────
            std::vector<uint8_t> pkt(kRtpHeaderSize + nal_size);
            build_rtp_header(pkt.data(), marker, pt, video_seq_++, rtp_ts, video_ssrc_);
            std::memcpy(pkt.data() + kRtpHeaderSize, pos, nal_size);
            if (fec_params_.enabled)
                fec_push_udp(video_fd_, client_video_, pkt.data(), pkt.size());
            else
                udp_send(video_fd_, client_video_, pkt.data(), pkt.size());
            ++video_packets_sent_;
        } else {
            // ── FU-A fragmentation ────────────────────────────────────────
            // RFC 6184 §5.8: FU indicator + FU header + payload
            const uint8_t nal_header = pos[0];
            const uint8_t nal_type   = nal_header & 0x1F;
            const uint8_t nri        = nal_header & 0x60;
            const uint8_t fu_ind     = nri | 28u; // type 28 = FU-A

            const uint8_t* src  = pos + 1; // skip original NAL header
            size_t         rem  = nal_size - 1;
            bool           first = true;

            while (rem > 0) {
                const size_t chunk = std::min(rem, kMTU - 2); // -2 for FU ind+hdr
                const bool   end   = (rem == chunk);
                const bool   last_pkt = end && last_nal;

                uint8_t fu_hdr = nal_type;
                if (first) fu_hdr |= 0x80; // S bit
                if (end)   fu_hdr |= 0x40; // E bit

                std::vector<uint8_t> pkt(kRtpHeaderSize + 2 + chunk);
                build_rtp_header(pkt.data(), last_pkt, pt, video_seq_++, rtp_ts, video_ssrc_);
                pkt[kRtpHeaderSize]     = fu_ind;
                pkt[kRtpHeaderSize + 1] = fu_hdr;
                std::memcpy(pkt.data() + kRtpHeaderSize + 2, src, chunk);

                if (fec_params_.enabled)
                    fec_push_udp(video_fd_, client_video_, pkt.data(), pkt.size());
                else
                    udp_send(video_fd_, client_video_, pkt.data(), pkt.size());
                ++video_packets_sent_;

                src   += chunk;
                rem   -= chunk;
                first  = false;
            }
        }

        pos  += nal_size;
        left -= nal_size;
    }
}

// ─── Opus RTP packetization ───────────────────────────────────────────────────
void RtpTransport::send_rtp_audio(const uint8_t* data, size_t size, int64_t pts_us) {
    if (!client_known_ || audio_fd_ < 0 || !data || size == 0) return;
    // Audio RTP timestamp: 48 kHz clock
    const uint32_t rtp_ts = static_cast<uint32_t>(pts_us * 48 / 1000);
    std::vector<uint8_t> pkt(kRtpHeaderSize + size);
    build_rtp_header(pkt.data(), true /*marker*/, kPtOpus, audio_seq_++, rtp_ts, audio_ssrc_);
    std::memcpy(pkt.data() + kRtpHeaderSize, data, size);
    udp_send(audio_fd_, client_audio_, pkt.data(), pkt.size());
    ++audio_packets_sent_;
}

// ─── ITransport send ──────────────────────────────────────────────────────────
void RtpTransport::send(pulsar::core::EncodedPacket packet) {
    std::lock_guard<std::mutex> lk(send_mtx_);
    if (!packet.buffer) return;
    send_rtp_video(packet.buffer->data(), packet.buffer->size(),
                   packet.pts_us, packet.is_keyframe, packet.codec);
}

void RtpTransport::send_batch(std::vector<pulsar::core::EncodedPacket> packets) {
    for (auto& p : packets) send(std::move(p));
}

void RtpTransport::send_audio(pulsar::core::AudioPacket packet) {
    std::lock_guard<std::mutex> lk(send_mtx_);
    if (!packet.data || packet.size == 0) return;
    send_rtp_audio(packet.data.get(), packet.size, packet.pts_us);
}

// ─── FEC ──────────────────────────────────────────────────────────────────────
void RtpTransport::fec_push_udp(int fd, const sockaddr_in& dest,
                                 const uint8_t* data, size_t size) {
    udp_send(fd, dest, data, size);
    const int shards = fec_params_.data_shards > 0 ? fec_params_.data_shards : 10;
    if (fec_parity_.size() < size) fec_parity_.resize(size, 0);
    for (size_t i = 0; i < size; ++i) fec_parity_[i] ^= data[i];
    fec_window_.push_back(std::vector<uint8_t>(data, data + size));
    if (static_cast<int>(fec_window_.size()) >= shards)
        fec_flush_parity(fd, dest);
}

void RtpTransport::fec_flush_parity(int fd, const sockaddr_in& dest) {
    if (fec_parity_.empty()) return;
    // Tag parity with 0xFF marker byte so receiver can identify it.
    std::vector<uint8_t> tagged(1 + fec_parity_.size());
    tagged[0] = 0xFF;
    std::memcpy(tagged.data() + 1, fec_parity_.data(), fec_parity_.size());
    udp_send(fd, dest, tagged.data(), tagged.size());
    fec_window_.clear();
    fec_parity_.clear();
}

// ─── SDP generation ───────────────────────────────────────────────────────────
void RtpTransport::print_sdp(const std::string& server_ip) const {
    std::cout
        << "v=0\n"
        << "o=pulsar 0 0 IN IP4 " << server_ip << "\n"
        << "s=Pulsar Stream\n"
        << "c=IN IP4 " << server_ip << "\n"
        << "t=0 0\n"
        << "m=video " << kDefaultVideoPort << " RTP/AVP " << (int)kPtH264 << "\n"
        << "a=rtpmap:" << (int)kPtH264 << " H264/90000\n"
        << "a=fmtp:"   << (int)kPtH264 << " profile-level-id=42001f;packetization-mode=1\n"
        << "a=recvonly\n"
        << "m=audio " << kDefaultAudioPort << " RTP/AVP " << (int)kPtOpus << "\n"
        << "a=rtpmap:" << (int)kPtOpus << " opus/48000/2\n"
        << "a=fmtp:"   << (int)kPtOpus << " minptime=10;useinbandfec=1\n"
        << "a=recvonly\n";
}

// ─── Interface stubs ──────────────────────────────────────────────────────────
void RtpTransport::set_event_callback(std::function<void(pulsar::core::TransportEvent)> cb) { event_cb_=std::move(cb); }
void RtpTransport::set_stats_callback(std::function<void(pulsar::core::NetworkStats)>   cb) { stats_cb_=std::move(cb); }
void RtpTransport::set_input_callback(std::function<void(pulsar::core::InputEvent)>     cb) { input_cb_=std::move(cb); }
void RtpTransport::set_mic_callback(std::function<void(pulsar::core::AudioFrame)>       cb) { mic_cb_=std::move(cb); }
void RtpTransport::set_jitter_buffer(int,int)                                              {}
void RtpTransport::set_fec_params(const pulsar::core::FecParams& p) {
    std::lock_guard<std::mutex> lk(send_mtx_);
    fec_params_=p; fec_window_.clear(); fec_parity_.clear();
}
void RtpTransport::send_haptic(const pulsar::core::HapticCommand&)                        {}
void RtpTransport::send_stats(const pulsar::core::PipelineMetrics&)                       {}
std::string RtpTransport::sink_id()   const { return "rtp"; }
void RtpTransport::on_packet(pulsar::core::EncodedPacket p) { send(std::move(p)); }
void RtpTransport::on_audio(pulsar::core::AudioPacket p)    { send_audio(std::move(p)); }
bool RtpTransport::connected()             const { return client_known_; }
size_t RtpTransport::video_packets_sent()  const { return video_packets_sent_; }
size_t RtpTransport::audio_packets_sent()  const { return audio_packets_sent_; }

} // namespace pulsar::transport::rtp
