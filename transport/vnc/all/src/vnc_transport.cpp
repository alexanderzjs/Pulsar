// transport/vnc/src/vnc_transport.cpp
// VNC Transport — RFB 3.8 server-side implementation (RFC 6143).
//
// Handshake sequence:
//   1. Server → Client : ProtocolVersion "RFB 003.008\n"
//   2. Client → Server : ProtocolVersion "RFB 003.008\n" (echo)
//   3. Server → Client : SecurityType list (1=None | 2=VNC Auth)
//   4. Client → Server : Selected SecurityType
//   5. If VNC Auth selected:
//        Server → Client : 16-byte challenge
//        Client → Server : 16-byte DES response
//        Server → Client : SecurityResult (0=OK, 1=fail)
//   6. Client → Server : ClientInit (shared-flag)
//   7. Server → Client : ServerInit (width, height, pixel-format, name)
//   — steady state —
//   8. Server → Client : FramebufferUpdate PDUs (produced by VncEncoder)
//   9. Client → Server : FramebufferUpdateRequest / KeyEvent / PointerEvent
//
// VNC Auth (type 2) uses DES-ECB with the 8-byte password as key (RFC 6143
// §5.2.2).  We provide a minimal 8-round DES implementation so there is no
// external dependency.

#include "vnc_transport.h"

#include "input.h"

#include <arpa/inet.h>
#include <atomic>
#include <cstring>
#include <cerrno>
#include <fcntl.h>
#include <functional>
#include <iostream>
#include <mutex>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace pulsar::transport::vnc {

// ─── Minimal DES for VNC Auth ─────────────────────────────────────────────────
//
// VNC uses single-DES ECB, 8-byte blocks, with the bit order of the password
// key bytes reversed (LSB first per byte — the original VNC quirk).

namespace des_impl {

// Permutation tables (standard DES, bit numbers 1-based)
static const uint8_t IP[64] = {
    58,50,42,34,26,18,10, 2, 60,52,44,36,28,20,12, 4,
    62,54,46,38,30,22,14, 6, 64,56,48,40,32,24,16, 8,
    57,49,41,33,25,17, 9, 1, 59,51,43,35,27,19,11, 3,
    61,53,45,37,29,21,13, 5, 63,55,47,39,31,23,15, 7
};
static const uint8_t FP[64] = {
    40, 8,48,16,56,24,64,32, 39, 7,47,15,55,23,63,31,
    38, 6,46,14,54,22,62,30, 37, 5,45,13,53,21,61,29,
    36, 4,44,12,52,20,60,28, 35, 3,43,11,51,19,59,27,
    34, 2,42,10,50,18,58,26, 33, 1,41, 9,49,17,57,25
};
static const uint8_t E[48] = {
    32, 1, 2, 3, 4, 5,  4, 5, 6, 7, 8, 9,
     8, 9,10,11,12,13, 12,13,14,15,16,17,
    16,17,18,19,20,21, 20,21,22,23,24,25,
    24,25,26,27,28,29, 28,29,30,31,32, 1
};
static const uint8_t P[32] = {
    16, 7,20,21, 29,12,28,17,  1,15,23,26,  5,18,31,10,
     2, 8,24,14, 32,27, 3, 9, 19,13,30, 6, 22,11, 4,25
};
static const uint8_t PC1[56] = {
    57,49,41,33,25,17, 9,  1,58,50,42,34,26,18,
    10, 2,59,51,43,35,27, 19,11, 3,60,52,44,36,
    63,55,47,39,31,23,15,  7,62,54,46,38,30,22,
    14, 6,61,53,45,37,29, 21,13, 5,28,20,12, 4
};
static const uint8_t PC2[48] = {
    14,17,11,24, 1, 5,  3,28,15, 6,21,10,
    23,19,12, 4,26, 8, 16, 7,27,20,13, 2,
    41,52,31,37,47,55, 30,40,51,45,33,48,
    44,49,39,56,34,53, 46,42,50,36,29,32
};
static const uint8_t SHIFTS[16] = {1,1,2,2,2,2,2,2,1,2,2,2,2,2,2,1};
static const uint8_t S[8][64] = {
    {14,4,13,1,2,15,11,8,3,10,6,12,5,9,0,7,
      0,15,7,4,14,2,13,1,10,6,12,11,9,5,3,8,
      4,1,14,8,13,6,2,11,15,12,9,7,3,10,5,0,
     15,12,8,2,4,9,1,7,5,11,3,14,10,0,6,13},
    {15,1,8,14,6,11,3,4,9,7,2,13,12,0,5,10,
      3,13,4,7,15,2,8,14,12,0,1,10,6,9,11,5,
      0,14,7,11,10,4,13,1,5,8,12,6,9,3,2,15,
     13,8,10,1,3,15,4,2,11,6,7,12,0,5,14,9},
    {10,0,9,14,6,3,15,5,1,13,12,7,11,4,2,8,
     13,7,0,9,3,4,6,10,2,8,5,14,12,11,15,1,
     13,6,4,9,8,15,3,0,11,1,2,12,5,10,14,7,
      1,10,13,0,6,9,8,7,4,15,14,3,11,5,2,12},
    {7,13,14,3,0,6,9,10,1,2,8,5,11,12,4,15,
     13,8,11,5,6,15,0,3,4,7,2,12,1,10,14,9,
     10,6,9,0,12,11,7,13,15,1,3,14,5,2,8,4,
      3,15,0,6,10,1,13,8,9,4,5,11,12,7,2,14},
    {2,12,4,1,7,10,11,6,8,5,3,15,13,0,14,9,
     14,11,2,12,4,7,13,1,5,0,15,10,3,9,8,6,
      4,2,1,11,10,13,7,8,15,9,12,5,6,3,0,14,
     11,8,12,7,1,14,2,13,6,15,0,9,10,4,5,3},
    {12,1,10,15,9,2,6,8,0,13,3,4,14,7,5,11,
     10,15,4,2,7,12,9,5,6,1,13,14,0,11,3,8,
      9,14,15,5,2,8,12,3,7,0,4,10,1,13,11,6,
      4,3,2,12,9,5,15,10,11,14,1,7,6,0,8,13},
    {4,11,2,14,15,0,8,13,3,12,9,7,5,10,6,1,
     13,0,11,7,4,9,1,10,14,3,5,12,2,15,8,6,
      1,4,11,13,12,3,7,14,10,15,6,8,0,5,9,2,
      6,11,13,8,1,4,10,7,9,5,0,15,14,2,3,12},
    {13,2,8,4,6,15,11,1,10,9,3,14,5,0,12,7,
      1,15,13,8,10,3,7,4,12,5,6,11,0,14,9,2,
      7,11,4,1,9,12,14,2,0,6,10,13,15,3,5,8,
      2,1,14,7,4,10,8,13,15,12,9,0,3,5,6,11}
};

static uint64_t perm(uint64_t in, const uint8_t* table, int n) {
    uint64_t out = 0;
    for (int i = 0; i < n; ++i) {
        const int bit = table[i] - 1;
        out = (out << 1) | ((in >> (63 - bit)) & 1);
    }
    return out;
}

static uint64_t des_block(uint64_t in, uint64_t subkeys[16]) {
    uint64_t block = perm(in, IP, 64);
    uint32_t L = static_cast<uint32_t>(block >> 32);
    uint32_t R = static_cast<uint32_t>(block);
    for (int r = 0; r < 16; ++r) {
        // Expand R to 48 bits
        uint64_t er = perm(static_cast<uint64_t>(R) << 32, E, 48);
        er ^= subkeys[r];
        // S-box substitution
        uint32_t S_out = 0;
        for (int s = 0; s < 8; ++s) {
            const uint8_t b6 = static_cast<uint8_t>((er >> (42 - 6*s)) & 0x3F);
            const uint8_t row = static_cast<uint8_t>((b6 & 0x20) >> 4 | (b6 & 0x01));
            const uint8_t col = static_cast<uint8_t>((b6 >> 1) & 0x0F);
            S_out = (S_out << 4) | S[s][row * 16 + col];
        }
        uint32_t f = static_cast<uint32_t>(perm(static_cast<uint64_t>(S_out) << 32, P, 32));
        uint32_t newR = L ^ f;
        L = R; R = newR;
    }
    uint64_t out = (static_cast<uint64_t>(R) << 32) | L;
    return perm(out, FP, 64);
}

// Encrypt 8-byte block with 8-byte key (VNC DES quirk: key bytes bit-reversed)
static void des_encrypt(const uint8_t key8[8], const uint8_t in[8], uint8_t out[8]) {
    // Reverse bit order in each key byte (VNC quirk)
    uint8_t key_rev[8];
    for (int i = 0; i < 8; ++i) {
        uint8_t b = key8[i];
        b = static_cast<uint8_t>(((b & 0x01) << 7) | ((b & 0x02) << 5) |
                                  ((b & 0x04) << 3) | ((b & 0x08) << 1) |
                                  ((b & 0x10) >> 1) | ((b & 0x20) >> 3) |
                                  ((b & 0x40) >> 5) | ((b & 0x80) >> 7));
        key_rev[i] = b;
    }
    // Build 64-bit key
    uint64_t K = 0;
    for (int i = 0; i < 8; ++i) K = (K << 8) | key_rev[i];
    // Generate 16 subkeys
    uint64_t C = perm(K, PC1, 56) >> 8; // 28-bit halves
    // PC1 gives 56 bits; store as two 28-bit halves in top bits
    uint64_t full = perm(K, PC1, 56);
    uint32_t Cl = static_cast<uint32_t>(full >> 28) & 0x0FFFFFFF;
    uint32_t Cr = static_cast<uint32_t>(full)       & 0x0FFFFFFF;
    (void)C;

    uint64_t subkeys[16];
    for (int r = 0; r < 16; ++r) {
        Cl = ((Cl << SHIFTS[r]) | (Cl >> (28 - SHIFTS[r]))) & 0x0FFFFFFF;
        Cr = ((Cr << SHIFTS[r]) | (Cr >> (28 - SHIFTS[r]))) & 0x0FFFFFFF;
        uint64_t CD = (static_cast<uint64_t>(Cl) << 28) | Cr;
        subkeys[r] = perm(CD << 8, PC2, 48); // shift to align 56-bit input
    }
    // Encrypt
    uint64_t block = 0;
    for (int i = 0; i < 8; ++i) block = (block << 8) | in[i];
    uint64_t cipher = des_block(block, subkeys);
    for (int i = 7; i >= 0; --i) { out[i] = static_cast<uint8_t>(cipher); cipher >>= 8; }
}

} // namespace des_impl

// ─── Wire helpers ─────────────────────────────────────────────────────────────

static void push16be(std::vector<uint8_t>& v, uint16_t x) {
    v.push_back(static_cast<uint8_t>(x >> 8));
    v.push_back(static_cast<uint8_t>(x));
}
static void push32be(std::vector<uint8_t>& v, uint32_t x) {
    v.push_back(static_cast<uint8_t>(x >> 24));
    v.push_back(static_cast<uint8_t>(x >> 16));
    v.push_back(static_cast<uint8_t>(x >> 8));
    v.push_back(static_cast<uint8_t>(x));
}
static uint16_t read16be(const uint8_t* p) {
    return (static_cast<uint16_t>(p[0]) << 8) | p[1];
}

static bool send_all(int fd, const void* data, size_t n) {
    const auto* p = static_cast<const uint8_t*>(data);
    size_t sent = 0;
    while (sent < n) {
        ssize_t r = ::send(fd, p + sent, n - sent, MSG_NOSIGNAL);
        if (r <= 0) return false;
        sent += static_cast<size_t>(r);
    }
    return true;
}
static bool send_all(int fd, const std::vector<uint8_t>& v) {
    return send_all(fd, v.data(), v.size());
}
static bool recv_all(int fd, uint8_t* buf, size_t n) {
    size_t got = 0;
    while (got < n) {
        ssize_t r = ::recv(fd, buf + got, n - got, 0);
        if (r <= 0) return false;
        got += static_cast<size_t>(r);
    }
    return true;
}

// ─── Impl ─────────────────────────────────────────────────────────────────────

struct VncTransportImpl {
    int listen_fd  = -1;
    int client_fd  = -1;
    std::atomic<bool> client_known{false};

    std::string password;   // empty = None auth
    uint16_t fb_width  = 1280;
    uint16_t fb_height = 720;

    std::mutex  send_mtx;
    std::thread input_th;
    std::atomic<bool> stop_input{false};

    std::function<void(pulsar::core::TransportEvent)> event_cb;
    std::function<void(pulsar::core::InputEvent)>     input_cb;

    ~VncTransportImpl() {
        stop_input.store(true);
        if (input_th.joinable()) input_th.join();
        if (client_fd >= 0) { ::close(client_fd); client_fd = -1; }
        if (listen_fd >= 0) { ::close(listen_fd); listen_fd = -1; }
    }
};

VncTransport::VncTransport() : impl_(std::make_unique<VncTransportImpl>()) {}
VncTransport::~VncTransport() = default;

void VncTransport::set_password(const std::string& pw) { impl_->password = pw; }

// ─── RFB Handshake ────────────────────────────────────────────────────────────

static bool do_rfb_handshake(VncTransportImpl& impl) {
    int fd = impl.client_fd;

    // 1. Send ProtocolVersion.
    const char* version = "RFB 003.008\n";
    if (!send_all(fd, version, 12)) return false;

    // 2. Receive client ProtocolVersion.
    uint8_t client_ver[12];
    if (!recv_all(fd, client_ver, 12)) return false;

    // 3. Send security-type list.
    const bool use_auth = !impl.password.empty();
    {
        std::vector<uint8_t> sec;
        sec.push_back(use_auth ? 1 : 1);   // number-of-security-types
        sec.push_back(use_auth ? 2 : 1);   // 2=VNC Auth, 1=None
        if (!send_all(fd, sec)) return false;
    }

    // 4. Receive selected security type.
    uint8_t selected;
    if (!recv_all(fd, &selected, 1)) return false;

    if (selected == 2 && use_auth) {
        // 5a. VNC Auth challenge-response (RFC 6143 §5.2.2)
        uint8_t challenge[16];
        // Generate a pseudo-random challenge from /dev/urandom.
        int urfd = ::open("/dev/urandom", O_RDONLY);
        if (urfd >= 0) {
            ssize_t n = ::read(urfd, challenge, 16);
            (void)n;
            ::close(urfd);
        } else {
            // Fallback: deterministic (insecure but functional for testing).
            for (int i = 0; i < 16; ++i) challenge[i] = static_cast<uint8_t>(i ^ 0xA5);
        }
        if (!send_all(fd, challenge, 16)) return false;

        uint8_t response[16];
        if (!recv_all(fd, response, 16)) return false;

        // Verify: DES-encrypt challenge with password key and compare.
        uint8_t key8[8] = {};
        const auto& pw = impl.password;
        for (int i = 0; i < 8 && i < static_cast<int>(pw.size()); ++i)
            key8[i] = static_cast<uint8_t>(pw[i]);

        uint8_t expected[16];
        des_impl::des_encrypt(key8, challenge,     expected);
        des_impl::des_encrypt(key8, challenge + 8, expected + 8);

        const bool ok = (std::memcmp(response, expected, 16) == 0);
        // Send SecurityResult: 0=OK, 1=fail.
        uint8_t result[4] = { 0, 0, 0, static_cast<uint8_t>(ok ? 0 : 1) };
        if (!send_all(fd, result, 4)) return false;
        if (!ok) {
            std::cerr << "[vnc] authentication failed\n";
            return false;
        }
    } else if (selected == 1) {
        // None: send SecurityResult OK.
        uint8_t result[4] = {0, 0, 0, 0};
        if (!send_all(fd, result, 4)) return false;
    } else {
        return false;
    }

    // 6. Receive ClientInit (shared-desktop flag, 1 byte).
    uint8_t client_init;
    if (!recv_all(fd, &client_init, 1)) return false;

    // 7. Send ServerInit.
    {
        std::vector<uint8_t> si;
        push16be(si, impl.fb_width);
        push16be(si, impl.fb_height);
        // PixelFormat (16 bytes): 32bpp, 24 depth, big-endian=0, true-colour=1,
        //   R/G/B max 255, shifts R=16, G=8, B=0, padding 3 bytes.
        si.push_back(32);  // bits-per-pixel
        si.push_back(24);  // depth
        si.push_back(0);   // big-endian-flag = 0 (little-endian)
        si.push_back(1);   // true-colour-flag
        push16be(si, 255); // red-max
        push16be(si, 255); // green-max
        push16be(si, 255); // blue-max
        si.push_back(16);  // red-shift   (BGRA: B=0, G=8, R=16)
        si.push_back(8);   // green-shift
        si.push_back(0);   // blue-shift
        si.push_back(0); si.push_back(0); si.push_back(0); // padding
        // name
        const char* name = "Pulsar";
        push32be(si, static_cast<uint32_t>(std::strlen(name)));
        for (const char* c = name; *c; ++c) si.push_back(static_cast<uint8_t>(*c));
        if (!send_all(fd, si)) return false;
    }

    std::cerr << "[vnc] handshake complete\n";
    return true;
}

// ─── Input receive thread ─────────────────────────────────────────────────────

// Sentinel type value to signal client disconnection from parse_rfb_message.
static constexpr auto kDisconnect = static_cast<pulsar::core::InputEvent::Type>(-1);
static constexpr auto kNone       = static_cast<pulsar::core::InputEvent::Type>(-2);

static pulsar::core::InputEvent parse_rfb_message(int fd) {
    pulsar::core::InputEvent ev{};
    ev.type = kNone;
    uint8_t msg_type;
    if (::recv(fd, &msg_type, 1, 0) <= 0) {
        ev.type = kDisconnect;
        return ev;
    }
    switch (msg_type) {
    case 0: { // SetPixelFormat (20 bytes payload)
        uint8_t buf[19]; recv_all(fd, buf, 19); break;
    }
    case 2: { // SetEncodings
        uint8_t hdr[3]; recv_all(fd, hdr, 3);
        const uint16_t num = read16be(hdr + 1);
        std::vector<uint8_t> encs(static_cast<size_t>(num) * 4);
        recv_all(fd, encs.data(), encs.size());
        break;
    }
    case 3: { // FramebufferUpdateRequest (9 bytes: incremental + x+y+w+h)
        uint8_t buf[9]; recv_all(fd, buf, 9); break;
    }
    case 4: { // KeyEvent (7 bytes: down-flag + padding2 + keysym4)
        uint8_t buf[7]; recv_all(fd, buf, 7);
        // keysym is at bytes 3-6 (big-endian)
        const uint32_t keysym = (static_cast<uint32_t>(buf[3]) << 24)
                              | (static_cast<uint32_t>(buf[4]) << 16)
                              | (static_cast<uint32_t>(buf[5]) << 8)
                              |  static_cast<uint32_t>(buf[6]);
        ev.type  = (buf[0] != 0) ? pulsar::core::InputEvent::Type::KeyDown
                                 : pulsar::core::InputEvent::Type::KeyUp;
        ev.code  = static_cast<int32_t>(keysym & 0xFFFF);
        break;
    }
    case 5: { // PointerEvent (5 bytes: button-mask + x2 + y2)
        uint8_t buf[5]; recv_all(fd, buf, 5);
        const uint16_t px = read16be(buf + 1);
        const uint16_t py = read16be(buf + 3);
        ev.code = static_cast<int32_t>(px) | (static_cast<int32_t>(py) << 16);
        if (buf[0] & 0x01) {
            ev.type  = pulsar::core::InputEvent::Type::MouseButton;
            ev.value = 1; // button 1 pressed
        } else {
            ev.type = pulsar::core::InputEvent::Type::MouseMove;
        }
        break;
    }
    case 6: { // ClientCutText
        uint8_t hdr[7]; recv_all(fd, hdr, 7);
        uint32_t len = (static_cast<uint32_t>(hdr[3]) << 24)
                     | (static_cast<uint32_t>(hdr[4]) << 16)
                     | (static_cast<uint32_t>(hdr[5]) << 8)
                     |  static_cast<uint32_t>(hdr[6]);
        if (len > 0 && len < 1024*1024) {
            std::vector<uint8_t> text(len);
            recv_all(fd, text.data(), len);
        }
        break;
    }
    default:
        break;
    }
    return ev;
}

static void vnc_input_loop(VncTransportImpl& impl) {
    while (!impl.stop_input.load()) {
        if (impl.client_fd < 0) break;
        fd_set rset; FD_ZERO(&rset); FD_SET(impl.client_fd, &rset);
        timeval tv{0, 100000};
        if (::select(impl.client_fd + 1, &rset, nullptr, nullptr, &tv) <= 0) continue;

        auto ev = parse_rfb_message(impl.client_fd);
        if (ev.type == kDisconnect) {
            if (impl.event_cb)
                impl.event_cb(pulsar::core::TransportEvent::Disconnected);
            break;
        }
        if (ev.type != kNone && impl.input_cb)
            impl.input_cb(ev);
    }
}

// ─── server_bind / wait_for_client ───────────────────────────────────────────

bool VncTransport::server_bind(int port) {
    impl_->listen_fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (impl_->listen_fd < 0) return false;
    int opt = 1;
    ::setsockopt(impl_->listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    ::setsockopt(impl_->listen_fd, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port        = htons(static_cast<uint16_t>(port));
    if (::bind(impl_->listen_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0
     || ::listen(impl_->listen_fd, 4) != 0) {
        ::close(impl_->listen_fd); impl_->listen_fd = -1;
        return false;
    }
    std::cerr << "[vnc] listening on port " << port << "\n";
    return true;
}

bool VncTransport::wait_for_client(int timeout_ms) {
    if (impl_->listen_fd < 0) return false;
    fd_set rset; FD_ZERO(&rset); FD_SET(impl_->listen_fd, &rset);
    timeval tv{ .tv_sec = timeout_ms / 1000, .tv_usec = (timeout_ms % 1000) * 1000 };
    if (::select(impl_->listen_fd + 1, &rset, nullptr, nullptr, &tv) <= 0) return false;

    sockaddr_in peer{}; socklen_t plen = sizeof(peer);
    impl_->client_fd = ::accept(impl_->listen_fd,
                                reinterpret_cast<sockaddr*>(&peer), &plen);
    if (impl_->client_fd < 0) return false;

    int opt = 1;
    ::setsockopt(impl_->client_fd, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt));

    if (!do_rfb_handshake(*impl_)) {
        ::close(impl_->client_fd); impl_->client_fd = -1;
        return false;
    }

    impl_->client_known.store(true);
    impl_->stop_input.store(false);
    impl_->input_th = std::thread([this] { vnc_input_loop(*impl_); });

    if (impl_->event_cb)
        impl_->event_cb(pulsar::core::TransportEvent::Ready);
    return true;
}

bool VncTransport::connect(const std::string& /*endpoint*/) {
    impl_->client_known.store(true);
    if (impl_->event_cb)
        impl_->event_cb(pulsar::core::TransportEvent::Ready);
    return true;
}

void VncTransport::disconnect() {
    impl_->stop_input.store(true);
    if (impl_->input_th.joinable()) impl_->input_th.join();
    if (impl_->client_fd >= 0) { ::close(impl_->client_fd); impl_->client_fd = -1; }
    impl_->client_known.store(false);
    if (impl_->event_cb)
        impl_->event_cb(pulsar::core::TransportEvent::Disconnected);
}

// ─── send ─────────────────────────────────────────────────────────────────────
//
// VncEncoder already produces a complete RFB FramebufferUpdate PDU.
// Just write it to the TCP socket.

void VncTransport::send(pulsar::core::EncodedPacket packet) {
    std::lock_guard<std::mutex> lk(impl_->send_mtx);
    if (!impl_->client_known.load() || impl_->client_fd < 0) return;
    if (!packet.buffer) return;
    send_all(impl_->client_fd, packet.buffer->data(), packet.buffer->size());
}

void VncTransport::send_batch(std::vector<pulsar::core::EncodedPacket> packets) {
    for (auto& p : packets) send(std::move(p));
}

// ─── Stubs ────────────────────────────────────────────────────────────────────

void VncTransport::send_audio(pulsar::core::AudioPacket) {}
void VncTransport::send_haptic(const pulsar::core::HapticCommand&) {}
void VncTransport::send_stats(const pulsar::core::PipelineMetrics&) {}
void VncTransport::set_stats_callback(std::function<void(pulsar::core::NetworkStats)>) {}
void VncTransport::set_jitter_buffer(int, int) {}
void VncTransport::set_fec_params(const pulsar::core::FecParams&) {}
void VncTransport::set_mic_callback(std::function<void(pulsar::core::AudioFrame)>) {}

void VncTransport::set_event_callback(
        std::function<void(pulsar::core::TransportEvent)> cb) {
    impl_->event_cb = std::move(cb);
}
void VncTransport::set_input_callback(
        std::function<void(pulsar::core::InputEvent)> cb) {
    impl_->input_cb = std::move(cb);
}

} // namespace pulsar::transport::vnc
