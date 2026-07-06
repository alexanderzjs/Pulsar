// transport/webrtc/linux/src/webrtc_transport.cpp
// Full browser-compatible WebRTC transport:
//   ICE via libnice, DTLS via OpenSSL, media via libsrtp2
//
// Browser connection flow:
//   1. Browser sends SDP offer  → POST /offer  (via HTTP signaling in factory)
//   2. Server calls handle_offer(offer_sdp) → returns SDP answer
//   3. apply_sdp_answer(answer) installs ICE credentials
//   4. ICE connects (libnice STUN)
//   5. DTLS handshake (OpenSSL over ICE path, memory BIO bridge)
//   6. SRTP keys extracted from DTLS TLS keying material
//   7. Video frames → RTP packetized → SRTP encrypted → ICE sent

#include "webrtc_transport.h"

#include <algorithm>
#include <arpa/inet.h>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <deque>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <netinet/in.h>
#include <random>
#include <iomanip>
#include <sstream>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <vector>

// libnice ICE
#include <nice/nice.h>
#include <nice/agent.h>

// OpenSSL DTLS
#include <openssl/bio.h>
#include <openssl/err.h>
#include <openssl/pem.h>
#include <openssl/rand.h>
#include <openssl/rsa.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>

// libsrtp2
#include <srtp2/srtp.h>

namespace pulsar::transport::webrtc {

// ─── Constants ────────────────────────────────────────────────────────────────
static constexpr int  kVideoPayloadType = 96;
static constexpr uint32_t kVideoSsrc    = 0xDEADBEEF;
static constexpr size_t   kMaxRtpPkt    = 1200;
// SRTP_MAX_TRAILER_LEN from libsrtp2
static constexpr size_t   kSrtpOverhead = 16 + 4; // auth tag + RTCP padding

struct WebRtcTransport::Impl {
    // ICE
    GMainLoop*  gloop     = nullptr;
    GThread*    gthread   = nullptr;
    NiceAgent*  agent     = nullptr;
    guint       stream_id = 0;
    std::string local_ufrag, local_pwd;
    std::vector<std::string> local_candidates; // formatted as candidate lines

    // Signaling state
    std::string pending_offer_sdp; // browser's SDP offer waiting to be applied

    // DTLS / SRTP bridge
    std::deque<std::vector<uint8_t>> dtls_rx;  // packets from ICE → DTLS thread
    std::mutex  dtls_rx_mu;
    std::condition_variable dtls_rx_cv;

    SSL* ssl = nullptr;
    BIO* rbio = nullptr; // encrypted data written by us INTO SSL
    BIO* wbio = nullptr; // encrypted data SSL wants to SEND (we read and forward)

    srtp_t srtp_send = nullptr;
    std::mutex srtp_mu;

    // RTP state
    uint16_t seq       = 0;
    uint32_t rtp_ts    = 0;
    uint32_t rtp_ts_inc= 3000; // 90kHz, ~33ms at 30fps

    // State flags
    std::atomic<bool> ice_connected{false};
    std::atomic<bool> dtls_done{false};
    std::thread       dtls_thread;

    ~Impl() {
        ice_connected.store(false);
        dtls_done.store(false);
        {
            std::lock_guard lk(dtls_rx_mu);
            dtls_rx.push_back({}); // wake DTLS thread
        }
        dtls_rx_cv.notify_all();
        if (dtls_thread.joinable()) dtls_thread.join();

        if (ssl)  { SSL_free(ssl); ssl = nullptr; }
        if (srtp_send) { srtp_dealloc(srtp_send); srtp_send = nullptr; }
        if (agent) { g_object_unref(agent); agent = nullptr; }
        if (gloop) {
            g_main_loop_quit(gloop);
            if (gthread) g_thread_join(gthread);
            g_main_loop_unref(gloop);
            gloop = nullptr;
        }
    }
};

// ─── Certificate ──────────────────────────────────────────────────────────────
struct DtlsCert {
    SSL_CTX* ctx  = nullptr;
    X509*    cert = nullptr;
    EVP_PKEY* key = nullptr;
    std::string fingerprint; // "sha-256 AA:BB:..."

    DtlsCert() {
        // RSA key
        EVP_PKEY_CTX* pctx = EVP_PKEY_CTX_new_id(EVP_PKEY_RSA, nullptr);
        EVP_PKEY_keygen_init(pctx);
        EVP_PKEY_CTX_set_rsa_keygen_bits(pctx, 2048);
        EVP_PKEY_keygen(pctx, &key);
        EVP_PKEY_CTX_free(pctx);

        // Self-signed X.509
        cert = X509_new();
        ASN1_INTEGER_set(X509_get_serialNumber(cert), 42);
        X509_gmtime_adj(X509_get_notBefore(cert), 0);
        X509_gmtime_adj(X509_get_notAfter(cert), 365*24*3600);
        X509_set_pubkey(cert, key);
        X509_NAME* name = X509_get_subject_name(cert);
        X509_NAME_add_entry_by_txt(name,"CN",MBSTRING_ASC,
            (const unsigned char*)"pulsar",6,-1,0);
        X509_set_issuer_name(cert, name);
        X509_sign(cert, key, EVP_sha256());

        // SHA-256 fingerprint
        uint8_t md[32]; unsigned int mdlen;
        X509_digest(cert, EVP_sha256(), md, &mdlen);
        std::ostringstream fp;
        fp << "sha-256";
        for (unsigned i = 0; i < mdlen; ++i)
            fp << (i==0?" ":":") << std::hex << std::uppercase
               << std::setfill('0') << std::setw(2) << (int)md[i];
        fingerprint = fp.str();

        // SSL context
        ctx = SSL_CTX_new(DTLS_method());
        SSL_CTX_use_certificate(ctx, cert);
        SSL_CTX_use_PrivateKey(ctx, key);
        SSL_CTX_set_verify(ctx, SSL_VERIFY_NONE, nullptr);
        // SRTP profile
        SSL_CTX_set_tlsext_use_srtp(ctx, "SRTP_AES128_CM_SHA1_80");

        std::cerr << "[webrtc] DTLS cert fingerprint: " << fingerprint << "\n";
    }
    ~DtlsCert() {
        if (ctx)  SSL_CTX_free(ctx);
        if (cert) X509_free(cert);
        if (key)  EVP_PKEY_free(key);
    }
};

static DtlsCert& global_cert() {
    static DtlsCert cert;
    return cert;
}

// ─── Impl ─────────────────────────────────────────────────────────────────────


// ─── ICE callbacks ────────────────────────────────────────────────────────────
static void on_candidate_gathering_done(NiceAgent* /*a*/, guint /*s*/,
                                         gpointer d)
{
    auto* self = static_cast<WebRtcTransport*>(d);
    auto& m = *self->impl_;

    // Get ICE credentials
    gchar* ufrag = nullptr; gchar* pwd = nullptr;
    nice_agent_get_local_credentials(m.agent, m.stream_id, &ufrag, &pwd);
    if (ufrag) { m.local_ufrag = ufrag; g_free(ufrag); }
    if (pwd)   { m.local_pwd   = pwd;   g_free(pwd);   }

    // Get candidates
    GSList* cands = nice_agent_get_local_candidates(m.agent, m.stream_id, 1);
    for (GSList* l = cands; l; l = l->next) {
        NiceCandidate* c = static_cast<NiceCandidate*>(l->data);
        gchar* sdp = nice_agent_generate_local_candidate_sdp(m.agent, c);
        if (sdp) { m.local_candidates.push_back(sdp); g_free(sdp); }
        nice_candidate_free(c);
    }
    g_slist_free(cands);

    std::cerr << "[webrtc] ICE gathering done: "
              << m.local_candidates.size() << " candidates\n";
}

static void on_component_state(NiceAgent* /*a*/, guint /*s*/, guint comp,
                                guint state, gpointer d)
{
    auto* self = static_cast<WebRtcTransport*>(d);
    if (comp != 1) return;
    if (state == NICE_COMPONENT_STATE_READY) {
        std::cerr << "[webrtc] ICE connected — starting DTLS\n";
        self->impl_->ice_connected.store(true);
        self->impl_->dtls_rx_cv.notify_all();
    } else if (state == NICE_COMPONENT_STATE_FAILED) {
        std::cerr << "[webrtc] ICE failed\n";
        if (self->event_cb_)
            self->event_cb_(pulsar::core::TransportEvent::Disconnected);
    }
}

static void on_receive(NiceAgent* /*a*/, guint /*s*/, guint /*comp*/,
                        guint len, gchar* buf, gpointer d)
{
    auto* self = static_cast<WebRtcTransport*>(d);
    auto& m = *self->impl_;

    if (!m.dtls_done.load()) {
        // Feed into DTLS rx queue
        std::lock_guard lk(m.dtls_rx_mu);
        m.dtls_rx.emplace_back(reinterpret_cast<uint8_t*>(buf),
                                reinterpret_cast<uint8_t*>(buf) + len);
        m.dtls_rx_cv.notify_one();
    }
    // Post-DTLS: could decrypt SRTCP / DataChannel here (future)
}

// ─── DTLS thread ─────────────────────────────────────────────────────────────
static void run_dtls_thread(WebRtcTransport* self) {
    auto& m = *self->impl_;

    // Wait for ICE
    {
        std::unique_lock lk(m.dtls_rx_mu);
        m.dtls_rx_cv.wait(lk, [&m] {
            return m.ice_connected.load() || !m.dtls_rx.empty();
        });
    }
    if (!m.ice_connected.load()) return;

    // Create SSL object with memory BIOs
    m.ssl  = SSL_new(global_cert().ctx);
    m.rbio = BIO_new(BIO_s_mem());
    m.wbio = BIO_new(BIO_s_mem());
    BIO_set_mem_eof_return(m.rbio, -1);
    SSL_set_bio(m.ssl, m.rbio, m.wbio);
    SSL_set_accept_state(m.ssl); // server

    // DTLS handshake loop
    std::cerr << "[webrtc] DTLS handshake starting\n";
    while (!m.dtls_done.load()) {
        // Push any received packets from ICE into the read BIO
        {
            std::lock_guard lk(m.dtls_rx_mu);
            while (!m.dtls_rx.empty()) {
                auto& pkt = m.dtls_rx.front();
                if (!pkt.empty())
                    BIO_write(m.rbio, pkt.data(), (int)pkt.size());
                m.dtls_rx.pop_front();
            }
        }

        int r = SSL_do_handshake(m.ssl);

        // Send any pending DTLS output via ICE
        {
            char tmp[2048];
            int n;
            while ((n = BIO_read(m.wbio, tmp, sizeof(tmp))) > 0) {
                nice_agent_send(m.agent, m.stream_id, 1, n, tmp);
            }
        }

        if (r == 1) {
            std::cerr << "[webrtc] DTLS handshake complete\n";
            break;
        }
        int err = SSL_get_error(m.ssl, r);
        if (err != SSL_ERROR_WANT_READ && err != SSL_ERROR_WANT_WRITE) {
            std::cerr << "[webrtc] DTLS error " << err << "\n";
            return;
        }

        // Wait for more ICE data
        std::unique_lock lk(m.dtls_rx_mu);
        m.dtls_rx_cv.wait_for(lk, std::chrono::milliseconds(50));
    }

    // Extract SRTP keys via TLS keying material export
    // Layout: client_key(16) + server_key(16) + client_salt(14) + server_salt(14) = 60
    uint8_t material[60] = {};
    if (SSL_export_keying_material(m.ssl, material, sizeof(material),
            "EXTRACTOR-dtls_srtp", 19, nullptr, 0, 0) != 1) {
        std::cerr << "[webrtc] keying material export failed\n";
        return;
    }

    // server_key = material[16..31], server_salt = material[44..57]
    uint8_t server_key[30];
    memcpy(server_key,      material + 16, 16); // server write key
    memcpy(server_key + 16, material + 44, 14); // server write salt

    srtp_policy_t policy = {};
    srtp_crypto_policy_set_aes_cm_128_hmac_sha1_80(&policy.rtp);
    srtp_crypto_policy_set_aes_cm_128_hmac_sha1_80(&policy.rtcp);
    policy.ssrc.type  = ssrc_any_outbound;
    policy.key        = server_key;
    policy.next       = nullptr;

    static bool srtp_inited = false;
    if (!srtp_inited) { srtp_init(); srtp_inited = true; }

    srtp_t ctx;
    if (srtp_create(&ctx, &policy) != srtp_err_status_ok) {
        std::cerr << "[webrtc] srtp_create failed\n";
        return;
    }
    {
        std::lock_guard lk(m.srtp_mu);
        m.srtp_send = ctx;
    }
    m.dtls_done.store(true);
    std::cerr << "[webrtc] SRTP ready — media can flow\n";
    if (self->event_cb_)
        self->event_cb_(pulsar::core::TransportEvent::Ready);
    self->connected_.store(true);
}

// ─── SDP helpers ─────────────────────────────────────────────────────────────
static std::string build_sdp_answer(const std::string& mid,
                                     const std::string& ufrag,
                                     const std::string& pwd,
                                     const std::string& fingerprint,
                                     const std::vector<std::string>& cands)
{
    std::ostringstream s;
    s << "v=0\r\n"
      << "o=pulsar 1 1 IN IP4 0.0.0.0\r\n"
      << "s=-\r\n"
      << "t=0 0\r\n"
      << "a=group:BUNDLE " << mid << "\r\n"
      << "m=video 9 UDP/TLS/RTP/SAVPF " << kVideoPayloadType << "\r\n"
      << "c=IN IP4 0.0.0.0\r\n"
      << "a=rtcp:9 IN IP4 0.0.0.0\r\n"
      << "a=ice-ufrag:" << ufrag << "\r\n"
      << "a=ice-pwd:" << pwd << "\r\n"
      << "a=ice-options:trickle\r\n"
      << "a=fingerprint:" << fingerprint << "\r\n"
      << "a=setup:passive\r\n"  // server is passive (client was active)
      << "a=mid:" << mid << "\r\n"
      << "a=sendonly\r\n"
      << "a=rtcp-mux\r\n"
      << "a=rtpmap:" << kVideoPayloadType << " H264/90000\r\n"
      << "a=fmtp:" << kVideoPayloadType
      << " level-asymmetry-allowed=1;packetization-mode=1;"
         "profile-level-id=42001f\r\n"
      << "a=ssrc:" << kVideoSsrc << " cname:pulsar0\r\n";
    for (const auto& c : cands) s << "a=" << c << "\r\n";
    s << "a=end-of-candidates\r\n";
    return s.str();
}

// Parse mid from SDP offer (a=mid:xxx)
static std::string parse_mid(const std::string& sdp) {
    auto pos = sdp.find("a=mid:");
    if (pos == std::string::npos) return "0";
    auto end = sdp.find_first_of("\r\n", pos + 6);
    return sdp.substr(pos + 6, end - pos - 6);
}

// Parse remote ICE credentials and candidates from SDP offer
static void parse_remote_ice(NiceAgent* agent, guint stream_id,
                               const std::string& sdp)
{
    std::string ufrag, pwd;
    auto find_attr = [&](const std::string& key) -> std::string {
        auto pos = sdp.find("\na=" + key + ":");
        if (pos == std::string::npos) pos = sdp.find("\r\na=" + key + ":");
        if (pos == std::string::npos) return {};
        pos = sdp.find(key + ":") + key.size() + 1;
        auto end = sdp.find_first_of("\r\n", pos);
        return sdp.substr(pos, end - pos);
    };
    ufrag = find_attr("ice-ufrag");
    pwd   = find_attr("ice-pwd");

    if (!ufrag.empty() && !pwd.empty()) {
        nice_agent_set_remote_credentials(agent, stream_id,
            ufrag.c_str(), pwd.c_str());
    }

    // Parse candidates
    size_t pos = 0;
    while ((pos = sdp.find("a=candidate:", pos)) != std::string::npos) {
        auto end = sdp.find_first_of("\r\n", pos);
        std::string cand_line = sdp.substr(pos + 2, end - pos - 2); // "candidate:..."
        NiceCandidate* c = nice_agent_parse_remote_candidate_sdp(
            agent, stream_id, cand_line.c_str());
        if (c) {
            GSList* list = g_slist_prepend(nullptr, c);
            nice_agent_set_remote_candidates(agent, stream_id, 1, list);
            nice_candidate_free(c);
            g_slist_free(list);
        }
        pos = end;
    }
}

// ─── Construction ─────────────────────────────────────────────────────────────
WebRtcTransport::WebRtcTransport() : impl_(std::make_unique<Impl>()) {
    // Init global cert (and srtp) lazily
    (void)global_cert();
}

WebRtcTransport::~WebRtcTransport() { disconnect(); }

// ─── SDP offer/answer exchange ────────────────────────────────────────────────
std::string WebRtcTransport::generate_sdp_offer() {
    // Kept for interface compat — in browser flow, browser sends offer,
    // server answers via handle_offer_and_answer().
    return handle_offer_and_answer(""); // returns server offer if no browser offer
}

std::string WebRtcTransport::handle_offer_and_answer(const std::string& browser_offer)
{
    auto& m = *impl_;

    // Init libnice
    if (!m.gloop) {
        m.gloop  = g_main_loop_new(nullptr, FALSE);
        m.agent  = nice_agent_new(g_main_loop_get_context(m.gloop),
                                   NICE_COMPATIBILITY_RFC5245);
        g_object_set(G_OBJECT(m.agent),
                     "stun-server",      "stun.l.google.com",
                     "stun-server-port", 19302,
                     nullptr);
        g_object_set(G_OBJECT(m.agent),
                     "controlling-mode", FALSE,
                     nullptr);
        g_signal_connect(m.agent, "candidate-gathering-done",
            G_CALLBACK(on_candidate_gathering_done), this);
        g_signal_connect(m.agent, "component-state-changed",
            G_CALLBACK(on_component_state), this);

        m.stream_id = nice_agent_add_stream(m.agent, 1);
        nice_agent_set_stream_name(m.agent, m.stream_id, "video");
        nice_agent_attach_recv(m.agent, m.stream_id, 1,
            g_main_loop_get_context(m.gloop), on_receive, this);

        // Run GLib loop on a background thread
        struct GloopRunner {
            static gpointer run(gpointer d) {
                g_main_loop_run(static_cast<GMainLoop*>(d));
                return nullptr;
            }
        };
        m.gthread = g_thread_new("ice", GloopRunner::run, m.gloop);

        nice_agent_gather_candidates(m.agent, m.stream_id);

        // Wait for gathering to complete (up to 3s)
        for (int i = 0; i < 30 && m.local_candidates.empty(); ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    if (!browser_offer.empty()) {
        // Install remote ICE from browser's offer, then start DTLS thread
        parse_remote_ice(m.agent, m.stream_id, browser_offer);

        if (!m.dtls_thread.joinable())
            m.dtls_thread = std::thread(run_dtls_thread, this);

        // Return SDP answer
        const std::string mid = parse_mid(browser_offer);
        return build_sdp_answer(mid, m.local_ufrag, m.local_pwd,
                                 global_cert().fingerprint,
                                 m.local_candidates);
    }
    // No browser offer yet — just return server's ICE info as a simple offer
    return build_sdp_answer("0", m.local_ufrag, m.local_pwd,
                             global_cert().fingerprint,
                             m.local_candidates);
}

bool WebRtcTransport::apply_sdp_answer(const std::string& sdp) {
    // In browser-offer mode this is unused; kept for interface compat.
    return !sdp.empty();
}

bool WebRtcTransport::connect(const std::string& /*ep*/) {
    return !generate_sdp_offer().empty();
}

void WebRtcTransport::disconnect() {
    connected_.store(false);
    if (event_cb_) event_cb_(pulsar::core::TransportEvent::Disconnected);
    impl_ = std::make_unique<Impl>(); // reset all state
}

// ─── RTP packetization ────────────────────────────────────────────────────────
static void build_rtp_header(uint8_t* hdr, bool marker,
                               uint16_t seq, uint32_t ts, uint32_t ssrc)
{
    hdr[0] = 0x80;
    hdr[1] = (marker ? 0x80 : 0x00) | (kVideoPayloadType & 0x7F);
    hdr[2] = seq >> 8; hdr[3] = seq & 0xFF;
    hdr[4] = ts >> 24; hdr[5] = (ts>>16)&0xFF;
    hdr[6] = (ts>>8)&0xFF; hdr[7] = ts&0xFF;
    hdr[8] = ssrc>>24; hdr[9]=(ssrc>>16)&0xFF;
    hdr[10]=(ssrc>>8)&0xFF; hdr[11]=ssrc&0xFF;
}

static void send_rtp(WebRtcTransport::Impl& m, const uint8_t* nalu, size_t len,
                     bool last_in_frame)
{
    const size_t kRtpHdr = 12;

    auto send_pkt = [&](const uint8_t* payload, size_t plen, bool marker) {
        std::vector<uint8_t> pkt(kRtpHdr + plen + SRTP_MAX_TRAILER_LEN);
        build_rtp_header(pkt.data(), marker, m.seq++, m.rtp_ts, kVideoSsrc);
        memcpy(pkt.data() + kRtpHdr, payload, plen);
        int pkt_len = (int)(kRtpHdr + plen);

        std::lock_guard lk(m.srtp_mu);
        if (!m.srtp_send) return;
        if (srtp_protect(m.srtp_send, pkt.data(), &pkt_len) != srtp_err_status_ok)
            return;
        nice_agent_send(m.agent, m.stream_id, 1, pkt_len,
                         reinterpret_cast<const gchar*>(pkt.data()));
    };

    if (len <= kMaxRtpPkt) {
        // Single NALU packet
        send_pkt(nalu, len, last_in_frame);
    } else {
        // FU-A fragmentation
        const uint8_t nalu_type = nalu[0];
        const uint8_t fu_indicator = (nalu_type & 0xE0) | 28; // FU-A
        size_t offset = 1;
        bool first = true;
        while (offset < len) {
            size_t chunk = std::min(len - offset, kMaxRtpPkt - 2);
            bool last_chunk = (offset + chunk >= len);
            std::vector<uint8_t> fu(chunk + 2);
            fu[0] = fu_indicator;
            fu[1] = (first ? 0x80 : 0x00) | (last_chunk ? 0x40 : 0x00)
                    | (nalu_type & 0x1F);
            memcpy(fu.data() + 2, nalu + offset, chunk);
            send_pkt(fu.data(), fu.size(), last_chunk && last_in_frame);
            offset += chunk;
            first = false;
        }
    }
    if (last_in_frame) m.rtp_ts += m.rtp_ts_inc;
}

// ─── ITransport: send ─────────────────────────────────────────────────────────
void WebRtcTransport::send(pulsar::core::EncodedPacket pkt) {
    if (!connected_.load() || !pkt.buffer) return;
    Impl& m = *impl_;

    // Split H264 Annex-B stream (00 00 00 01 ...) into NALUs
    const uint8_t* data = pkt.buffer->data();
    size_t size         = pkt.buffer->size();

    // Find and send each NALU
    size_t start = 0;
    auto find_sc = [&](size_t from) -> size_t {
        for (size_t i = from; i + 3 < size; ++i) {
            if (data[i]==0 && data[i+1]==0 && data[i+2]==0 && data[i+3]==1)
                return i;
            if (data[i]==0 && data[i+2]==1 && i+2 < size) { /* 3-byte SC */ }
        }
        return size;
    };

    // Skip leading start codes
    while (start + 3 < size &&
           data[start]==0 && data[start+1]==0 && data[start+2]==0 &&
           data[start+3]==1) start += 4;

    size_t pos = start;
    while (pos < size) {
        size_t next_sc = find_sc(pos + 1);
        size_t nalu_len = (next_sc < size) ? next_sc - pos : size - pos;
        if (nalu_len > 0) {
            bool last = (next_sc >= size);
            send_rtp(m, data + pos, nalu_len, last);
        }
        if (next_sc >= size) break;
        pos = next_sc + 4;
    }
}

void WebRtcTransport::send_batch(std::vector<pulsar::core::EncodedPacket> pkts) {
    for (auto& p : pkts) send(std::move(p));
}

void WebRtcTransport::send_audio(pulsar::core::AudioPacket /*pkt*/) {
    // Audio over WebRTC requires a separate SRTP stream — future work.
}

// ─── Stubs ────────────────────────────────────────────────────────────────────
void WebRtcTransport::set_event_callback(std::function<void(pulsar::core::TransportEvent)> cb) { event_cb_=std::move(cb); }
void WebRtcTransport::set_stats_callback(std::function<void(pulsar::core::NetworkStats)> cb)   { stats_cb_=std::move(cb); }
void WebRtcTransport::set_input_callback(std::function<void(pulsar::core::InputEvent)> cb)     { input_cb_=std::move(cb); }
void WebRtcTransport::set_mic_callback(std::function<void(pulsar::core::AudioFrame)> cb)       { mic_cb_=std::move(cb); }
void WebRtcTransport::set_jitter_buffer(int,int)                                               {}
void WebRtcTransport::set_fec_params(const pulsar::core::FecParams& p){ fec_params_=p; }
void WebRtcTransport::send_haptic(const pulsar::core::HapticCommand&)                          {}
void WebRtcTransport::send_stats(const pulsar::core::PipelineMetrics&)                         {}
std::string WebRtcTransport::sink_id()  const { return "webrtc"; }
void WebRtcTransport::on_packet(pulsar::core::EncodedPacket p) { send(std::move(p)); }
void WebRtcTransport::on_audio(pulsar::core::AudioPacket p)    { send_audio(std::move(p)); }
bool WebRtcTransport::connected() const { return connected_.load(); }

} // namespace pulsar::transport::webrtc
