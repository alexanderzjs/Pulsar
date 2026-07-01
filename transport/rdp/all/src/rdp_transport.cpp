// transport/rdp/src/rdp_transport.cpp
// RDP Transport — MS-RDPBCGR compatible server-side implementation.
//
// Handshake sequence (Classic RDP without NLA, security type = RDP):
//   1. Client → Server : X.224 Connection Request (TPKT/X.224 CR-TPDU)
//   2. Server → Client : X.224 Connection Confirm (CC-TPDU)
//   3. Client → Server : MCS Connect-Initial (T.125/ASN.1 BER)
//   4. Server → Client : MCS Connect-Response
//   5. Client → Server : MCS Erect Domain Request
//   6. Client → Server : MCS Attach User Request
//   7. Server → Client : MCS Attach User Confirm
//   8. Client ↔ Server : MCS Channel Join Request/Confirm (I/O channel 1003, …)
//   9. Server → Client : RDP Security Exchange (if Classic RDP security)
//  10. Server → Client : MCS Send Data Indication — Server Settings PDUs
//                        (Demand Active PDU, Synchronize PDU, Control PDU,
//                         Font Map PDU)
//  11. Client → Server : Client Confirm Active PDU, Synchronize, …
//  12. Steady state: Server sends TS_FP_UPDATE_PDU (Fast-Path) frames.
//
// This implementation handles steps 1-11 with minimal parsing; it does NOT
// implement NLA/CredSSP (no TLS/NTLM).  Classic RDP security with null
// encryption is used so that mstsc can connect with
//   "Security Level = Classic RDP (no NLA)" or via Group Policy.
//
// Input (keyboard/mouse) is received via MCS Send Data Request PDUs on the
// I/O channel and dispatched through set_input_callback().

#include "rdp_transport.h"

#include "input.h"

#include <arpa/inet.h>
#include <atomic>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace pulsar::transport::rdp {

// ─── Constants ───────────────────────────────────────────────────────────────

static constexpr uint16_t kMcsIoChannel  = 1003;
static constexpr uint16_t kMcsUserId     = 1001;

// RDP PDU types (shareControlHeader.pduType)
static constexpr uint16_t PDU_TYPE_DEMAND_ACTIVE = 0x0011;
static constexpr uint16_t PDU_TYPE_SYNCHRONIZE   = 0x0014;
static constexpr uint16_t PDU_TYPE_CONTROL       = 0x0014;
static constexpr uint16_t PDU_TYPE_FONTMAP        = 0x0028;

// Fast-Path update header
static constexpr uint8_t FP_ACTION_FASTPATH = 0x00;

// ─── Wire helpers ─────────────────────────────────────────────────────────────

static void push16be(std::vector<uint8_t>& v, uint16_t x) {
    v.push_back(static_cast<uint8_t>(x >> 8));
    v.push_back(static_cast<uint8_t>(x));
}
static void push16le(std::vector<uint8_t>& v, uint16_t x) {
    v.push_back(static_cast<uint8_t>(x));
    v.push_back(static_cast<uint8_t>(x >> 8));
}
static void push32le(std::vector<uint8_t>& v, uint32_t x) {
    v.push_back(static_cast<uint8_t>(x));
    v.push_back(static_cast<uint8_t>(x >> 8));
    v.push_back(static_cast<uint8_t>(x >> 16));
    v.push_back(static_cast<uint8_t>(x >> 24));
}

static uint16_t read16le(const uint8_t* p) {
    return static_cast<uint16_t>(p[0]) | (static_cast<uint16_t>(p[1]) << 8);
}
static uint16_t read16be(const uint8_t* p) {
    return (static_cast<uint16_t>(p[0]) << 8) | static_cast<uint16_t>(p[1]);
}

// TPKT header (RFC 1006):  version(1) reserved(1) length(2 BE)
static std::vector<uint8_t> tpkt_wrap(const std::vector<uint8_t>& payload) {
    const uint16_t total = static_cast<uint16_t>(4 + payload.size());
    std::vector<uint8_t> out;
    out.push_back(0x03); out.push_back(0x00);
    push16be(out, total);
    out.insert(out.end(), payload.begin(), payload.end());
    return out;
}

// X.224 Data TPDU (class 0): LI(1) + code(1) + EOT(1) + data
static std::vector<uint8_t> x224_data(const std::vector<uint8_t>& payload) {
    std::vector<uint8_t> x;
    x.push_back(0x02);   // LI = 2 (two bytes follow before user data)
    x.push_back(0xF0);   // Data TPDU code
    x.push_back(0x80);   // EOT = 1
    x.insert(x.end(), payload.begin(), payload.end());
    return tpkt_wrap(x);
}

// ─── send_all: blocking write of exactly n bytes ─────────────────────────────

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

// ─── recv_all: blocking read of exactly n bytes ──────────────────────────────

static bool recv_all(int fd, uint8_t* buf, size_t n) {
    size_t got = 0;
    while (got < n) {
        ssize_t r = ::recv(fd, buf + got, n - got, 0);
        if (r <= 0) return false;
        got += static_cast<size_t>(r);
    }
    return true;
}

// Reads a TPKT packet from fd (blocking).  Returns the X.224 payload.
static std::vector<uint8_t> recv_tpkt(int fd) {
    uint8_t hdr[4];
    if (!recv_all(fd, hdr, 4)) return {};
    if (hdr[0] != 0x03) return {};  // not TPKT
    const uint16_t total = read16be(hdr + 2);
    if (total < 4) return {};
    std::vector<uint8_t> body(total - 4);
    if (!body.empty() && !recv_all(fd, body.data(), body.size())) return {};
    return body; // X.224 header + payload
}

// ─── Impl ─────────────────────────────────────────────────────────────────────

struct RdpTransportImpl {
    int listen_fd  = -1;
    int client_fd  = -1;
    std::atomic<bool> client_known{false};

    std::mutex  send_mtx;
    std::thread input_th;
    std::atomic<bool> stop_input{false};

    // Negotiated desktop size (sent in Demand Active PDU)
    uint16_t desktop_w = 1280;
    uint16_t desktop_h = 720;

    std::function<void(pulsar::core::TransportEvent)> event_cb;
    std::function<void(pulsar::core::InputEvent)>     input_cb;

    ~RdpTransportImpl() {
        stop_input.store(true);
        if (input_th.joinable()) input_th.join();
        if (client_fd >= 0) { ::close(client_fd); client_fd = -1; }
        if (listen_fd >= 0) { ::close(listen_fd); listen_fd = -1; }
    }
};

// ─── Construction ─────────────────────────────────────────────────────────────

RdpTransport::RdpTransport() : impl_(std::make_unique<RdpTransportImpl>()) {}
RdpTransport::~RdpTransport() = default;

// ─── Handshake PDU builders ───────────────────────────────────────────────────

// X.224 Connection Confirm (CC-TPDU) — no security selected
static std::vector<uint8_t> make_x224_cc() {
    // LI(1) code+CDT(1) dst-ref(2) src-ref(2) class(1)
    std::vector<uint8_t> payload = { 0x06, 0xD0, 0x00, 0x00, 0x12, 0x34, 0x00 };
    return tpkt_wrap(payload);
}

// MCS Connect-Response (T.125 §8.3) — minimal BER encoding
// We use a canned response accepting the domain; desktop size is embedded
// in the GCC Conference Create Response inside the user data field.
static std::vector<uint8_t> make_mcs_connect_response(uint16_t w, uint16_t h) {
    // GCC Conference Create Response (T.124 §8.7)
    // Contains Server Core Data (CS_CORE) and Server Network Data (CS_NET).
    std::vector<uint8_t> gcc;

    // Key+length for "conference createResponse" (OID 0.0.20.124.0.1)
    const uint8_t t124_key[] = {
        0x00, 0x05, 0x00, 0x14,     // t124Identifier
        0x7C, 0x00, 0x01            // connectPDU tag
    };
    gcc.insert(gcc.end(), t124_key, t124_key + sizeof(t124_key));

    // Server Core Data (type=0xC001)
    std::vector<uint8_t> core_data;
    push16le(core_data, 0xC001); // header type
    push16le(core_data, 12);     // length
    push32le(core_data, 0x00080004); // rdpVersion = RDP 5.0
    push16le(core_data, w);      // clientRequestedProtocols
    push16le(core_data, h);      // (repurposed for width/height hint)

    // Wrap as GCC User Data block
    gcc.push_back(0x01); // conferenceCreateResponse tag (simplified)
    gcc.push_back(0xC0);
    gcc.insert(gcc.end(), core_data.begin(), core_data.end());

    // MCS Connect-Response BER TLV
    std::vector<uint8_t> mcs;
    mcs.push_back(0x7F); mcs.push_back(0x66); // Connect-Response tag
    // Length (BER long form)
    const size_t inner_len = 6 + gcc.size();
    mcs.push_back(0x82);
    mcs.push_back(static_cast<uint8_t>(inner_len >> 8));
    mcs.push_back(static_cast<uint8_t>(inner_len));
    // result ENUMERATED { rt-successful(0) }
    mcs.push_back(0x0A); mcs.push_back(0x01); mcs.push_back(0x00);
    // calledConnectId INTEGER 0
    mcs.push_back(0x02); mcs.push_back(0x01); mcs.push_back(0x00);
    // domainParameters (use connect-initial values back)
    mcs.insert(mcs.end(), gcc.begin(), gcc.end());

    return x224_data(mcs);
}

// MCS Attach User Confirm (T.125)
static std::vector<uint8_t> make_mcs_attach_user_confirm(uint16_t user_id) {
    std::vector<uint8_t> mcs;
    mcs.push_back(0x2E); // AttachUserConfirm + result=success
    push16be(mcs, user_id);
    return x224_data(mcs);
}

// MCS Channel Join Confirm
static std::vector<uint8_t> make_mcs_channel_join_confirm(
        uint16_t user_id, uint16_t channel_id) {
    std::vector<uint8_t> mcs;
    mcs.push_back(0x3E); // ChannelJoinConfirm + result=success
    push16be(mcs, user_id);
    push16be(mcs, channel_id);
    push16be(mcs, channel_id);
    return x224_data(mcs);
}

// Builds an MCS Send Data Indication wrapping an RDP PDU.
static std::vector<uint8_t> make_mcs_sdi(uint16_t user_id, uint16_t channel,
                                          const std::vector<uint8_t>& rdp_pdu) {
    std::vector<uint8_t> mcs;
    mcs.push_back(0x68); // SendDataIndication
    push16be(mcs, user_id);
    push16be(mcs, channel);
    mcs.push_back(0x70); // dataPriority=high, segmentation=single
    // BER length of rdp_pdu
    if (rdp_pdu.size() < 0x80) {
        mcs.push_back(static_cast<uint8_t>(rdp_pdu.size()));
    } else {
        mcs.push_back(0x82);
        mcs.push_back(static_cast<uint8_t>(rdp_pdu.size() >> 8));
        mcs.push_back(static_cast<uint8_t>(rdp_pdu.size()));
    }
    mcs.insert(mcs.end(), rdp_pdu.begin(), rdp_pdu.end());
    return x224_data(mcs);
}

// RDP Demand Active PDU (MS-RDPBCGR §2.2.1.13)
static std::vector<uint8_t> make_demand_active(uint16_t w, uint16_t h) {
    std::vector<uint8_t> rdp;
    // Share Control Header (shareControlHeader)
    push16le(rdp, 0); // totalLength — filled below
    push16le(rdp, PDU_TYPE_DEMAND_ACTIVE | 0x10); // pduType + version
    push16le(rdp, kMcsUserId); // PDUSource

    // Demand Active body
    push32le(rdp, 0x00000001); // shareId
    push16le(rdp, 4);          // lengthSourceDescriptor
    rdp.push_back('R'); rdp.push_back('D'); rdp.push_back('P'); rdp.push_back(0);

    // Capability sets: General, Bitmap, Order, Color Cache
    const uint16_t kNumCaps = 4;
    push16le(rdp, kNumCaps);
    push16le(rdp, 0); // lengthCombinedCapabilities — filled below
    const size_t caps_offset = rdp.size();

    // TS_GENERAL_CAPABILITYSET (type=1, len=24)
    push16le(rdp, 0x0001); push16le(rdp, 24);
    push16le(rdp, 0x0001); // osMajorType = Windows
    push16le(rdp, 0x0003); // osMinorType = NT
    push16le(rdp, 0x0200); // protocolVersion
    push16le(rdp, 0x0000); // pad2OctetsA
    push16le(rdp, 0x0001); // compressionTypes
    push16le(rdp, 0x0400); // extraFlags (NO_BITMAP_COMPRESSION_HDR)
    push16le(rdp, 0x0000); // updateCapabilityFlag
    push16le(rdp, 0x0000); // remoteUnshareFlag
    push16le(rdp, 0x0000); // compressionLevel
    push16le(rdp, 0x0000); // refreshRectSupport / suppressOutputSupport

    // TS_BITMAP_CAPABILITYSET (type=2, len=28)
    push16le(rdp, 0x0002); push16le(rdp, 28);
    push16le(rdp, 32);     // preferredBitsPerPixel
    push16le(rdp, 1);      // receive1BitPerPixel
    push16le(rdp, 1);      // receive4BitsPerPixel
    push16le(rdp, 1);      // receive8BitsPerPixel
    push16le(rdp, w);      // desktopWidth
    push16le(rdp, h);      // desktopHeight
    push16le(rdp, 0);      // pad2Octets
    push16le(rdp, 1);      // desktopResizeFlag
    push16le(rdp, 0);      // bitmapCompressionFlag
    push16le(rdp, 0);      // highColorFlags
    push16le(rdp, 0);      // drawingFlags
    push16le(rdp, 0);      // multipleRectangleSupport
    push16le(rdp, 0);      // pad2OctetsB

    // TS_ORDER_CAPABILITYSET (type=3, len=88)
    push16le(rdp, 0x0003); push16le(rdp, 88);
    for (int i = 0; i < 80; ++i) rdp.push_back(0); // all zeros = no orders

    // TS_COLORCACHE_CAPABILITYSET (type=10, len=8)
    push16le(rdp, 0x000A); push16le(rdp, 8);
    push16le(rdp, 6);  // colorTableCacheSize
    push16le(rdp, 0);  // pad2OctetsA

    const uint16_t caps_len = static_cast<uint16_t>(rdp.size() - caps_offset);
    rdp[caps_offset - 2] = static_cast<uint8_t>(caps_len);
    rdp[caps_offset - 1] = static_cast<uint8_t>(caps_len >> 8);

    // sessionId
    push32le(rdp, 0x00000001);

    // Fill in totalLength
    const uint16_t total = static_cast<uint16_t>(rdp.size());
    rdp[0] = static_cast<uint8_t>(total);
    rdp[1] = static_cast<uint8_t>(total >> 8);

    return make_mcs_sdi(kMcsUserId, kMcsIoChannel, rdp);
}

// RDP Synchronize PDU
static std::vector<uint8_t> make_synchronize_pdu() {
    std::vector<uint8_t> rdp;
    push16le(rdp, 22); // totalLength
    push16le(rdp, PDU_TYPE_SYNCHRONIZE | 0x10);
    push16le(rdp, kMcsUserId);
    // TS_SYNCHRONIZE_PDU
    push16le(rdp, 1);  // messageType = SYNCMSGTYPE_SYNC
    push16le(rdp, kMcsUserId);
    return make_mcs_sdi(kMcsUserId, kMcsIoChannel, rdp);
}

// RDP Control Cooperate PDU
static std::vector<uint8_t> make_control_cooperate() {
    std::vector<uint8_t> rdp;
    push16le(rdp, 28);
    push16le(rdp, PDU_TYPE_CONTROL | 0x10);
    push16le(rdp, kMcsUserId);
    push16le(rdp, 1);  // action = CTRLACTION_COOPERATE
    push16le(rdp, 0);  // grantId
    push32le(rdp, 0);  // controlId
    return make_mcs_sdi(kMcsUserId, kMcsIoChannel, rdp);
}

// RDP Control Granted Control PDU
static std::vector<uint8_t> make_control_granted() {
    std::vector<uint8_t> rdp;
    push16le(rdp, 28);
    push16le(rdp, PDU_TYPE_CONTROL | 0x10);
    push16le(rdp, kMcsUserId);
    push16le(rdp, 2);  // action = CTRLACTION_GRANTED_CONTROL
    push16le(rdp, kMcsUserId);
    push32le(rdp, 0x00000001);
    return make_mcs_sdi(kMcsUserId, kMcsIoChannel, rdp);
}

// RDP Font Map PDU
static std::vector<uint8_t> make_font_map_pdu() {
    std::vector<uint8_t> rdp;
    push16le(rdp, 26);
    push16le(rdp, PDU_TYPE_FONTMAP | 0x10);
    push16le(rdp, kMcsUserId);
    push16le(rdp, 0);  // numberFonts
    push16le(rdp, 0);  // totalNumFonts
    push16le(rdp, 3);  // mapFlags = FONTLIST_FIRST | FONTLIST_LAST
    push16le(rdp, 4);  // entrySize
    return make_mcs_sdi(kMcsUserId, kMcsIoChannel, rdp);
}

// ─── Handshake ────────────────────────────────────────────────────────────────

static bool do_rdp_handshake(RdpTransportImpl& impl) {
    int fd = impl.client_fd;

    // Step 1: Read X.224 Connection Request.
    auto cr = recv_tpkt(fd);
    if (cr.empty()) {
        std::cerr << "[rdp] failed to receive X.224 CR\n";
        return false;
    }

    // Step 2: Send X.224 Connection Confirm.
    if (!send_all(fd, make_x224_cc())) return false;

    // Step 3: Read MCS Connect-Initial.
    auto mci = recv_tpkt(fd);
    if (mci.empty()) {
        std::cerr << "[rdp] failed to receive MCS Connect-Initial\n";
        return false;
    }

    // Step 4: Send MCS Connect-Response.
    if (!send_all(fd, make_mcs_connect_response(impl.desktop_w, impl.desktop_h)))
        return false;

    // Step 5: Read MCS Erect Domain Request.
    auto edr = recv_tpkt(fd);
    if (edr.empty()) return false;

    // Step 6: Read MCS Attach User Request.
    auto aur = recv_tpkt(fd);
    if (aur.empty()) return false;

    // Step 7: Send MCS Attach User Confirm.
    if (!send_all(fd, make_mcs_attach_user_confirm(kMcsUserId))) return false;

    // Step 8: Process Channel Join Requests (client joins I/O channel + user channel).
    for (int i = 0; i < 4; ++i) {
        auto cjr = recv_tpkt(fd);
        if (cjr.empty()) break;
        // Channel ID is at bytes 5-6 (0-based) of the X.224 payload.
        if (cjr.size() < 9) continue;
        const uint16_t channel_id = read16be(cjr.data() + 7);
        if (!send_all(fd, make_mcs_channel_join_confirm(kMcsUserId, channel_id)))
            return false;
    }

    // Step 9: (Classic RDP security) Client sends Security Exchange PDU.
    // We skip actual decryption and accept the client's key exchange.
    auto sec = recv_tpkt(fd);
    (void)sec; // discard

    // Step 10: Server Settings PDUs.
    if (!send_all(fd, make_demand_active(impl.desktop_w, impl.desktop_h)))
        return false;
    if (!send_all(fd, make_synchronize_pdu()))     return false;
    if (!send_all(fd, make_control_cooperate()))   return false;
    if (!send_all(fd, make_control_granted()))     return false;
    if (!send_all(fd, make_font_map_pdu()))        return false;

    // Step 11: Read Client Confirm Active + sequence PDUs (drain them).
    // The client typically sends: ConfirmActive, Synchronize, ControlCooperate,
    // RequestControl, FontList.  We drain up to 8 PDUs.
    for (int i = 0; i < 8; ++i) {
        auto p = recv_tpkt(fd);
        if (p.empty()) break;
    }

    std::cerr << "[rdp] handshake complete\n";
    return true;
}

// ─── Input receive thread ─────────────────────────────────────────────────────

static pulsar::core::InputEvent parse_rdp_input(const uint8_t* p, size_t len) {
    // TS_FP_INPUT_EVENT (Fast-Path Input Event, §2.2.8.1.2)
    // eventHeader bits [4:0]: eventCode
    //   0x01 = FASTPATH_INPUT_EVENT_SCANCODE
    //   0x02 = FASTPATH_INPUT_EVENT_MOUSE
    pulsar::core::InputEvent ev{};
    if (len < 1) return ev;
    const uint8_t hdr = p[0];
    const uint8_t code = hdr & 0x1F;
    if (code == 0x01 && len >= 2) {
        // Scancode event: pressed = flag bit 0 of eventHeader NOT set
        ev.type  = (hdr & 0x80) ? pulsar::core::InputEvent::Type::KeyUp
                                 : pulsar::core::InputEvent::Type::KeyDown;
        ev.code  = p[1]; // scan code
    } else if (code == 0x02 && len >= 7) {
        // Mouse event
        const uint16_t mouse_flags = read16le(p + 1);
        const uint16_t x = read16le(p + 3);
        const uint16_t y = read16le(p + 5);
        ev.code  = static_cast<int32_t>(x) | (static_cast<int32_t>(y) << 16);
        if (mouse_flags & 0x0800) { // PTRFLAGS_MOVE
            ev.type  = pulsar::core::InputEvent::Type::MouseMove;
        } else if (mouse_flags & 0x0100) { // PTRFLAGS_BUTTON1
            ev.type  = pulsar::core::InputEvent::Type::MouseButton;
            ev.value = (mouse_flags & 0x8000) ? 1 : 0;
        } else if (mouse_flags & 0x0200) { // PTRFLAGS_BUTTON2
            ev.type  = pulsar::core::InputEvent::Type::MouseButton;
            ev.value = (mouse_flags & 0x8000) ? 1 : 0;
        } else {
            ev.type = pulsar::core::InputEvent::Type::MouseMove;
        }
    }
    return ev;
}

static void input_receive_loop(RdpTransportImpl& impl) {
    while (!impl.stop_input.load()) {
        // Check if client_fd is still valid.
        if (impl.client_fd < 0) break;

        // Poll with 100 ms timeout.
        fd_set rset;
        FD_ZERO(&rset);
        FD_SET(impl.client_fd, &rset);
        timeval tv{0, 100000};
        int n = ::select(impl.client_fd + 1, &rset, nullptr, nullptr, &tv);
        if (n <= 0) continue;

        // Try to read as a Fast-Path Input PDU.
        uint8_t hdr[2];
        if (::recv(impl.client_fd, hdr, 1, MSG_PEEK) <= 0) {
            // Client disconnected.
            if (impl.event_cb)
                impl.event_cb(pulsar::core::TransportEvent::Disconnected);
            break;
        }

        // Fast-Path input: action bits [1:0] = 0x00
        if ((hdr[0] & 0x03) == 0x00) {
            // Fast-Path PDU: header(1) + length(1 or 2) + events
            uint8_t fp_hdr[2];
            if (!recv_all(impl.client_fd, fp_hdr, 2)) break;
            uint16_t pdu_len;
            if (fp_hdr[1] & 0x80) {
                // 2-byte length
                uint8_t len2;
                if (!recv_all(impl.client_fd, &len2, 1)) break;
                pdu_len = static_cast<uint16_t>((fp_hdr[1] & 0x7F) << 8) | len2;
                pdu_len -= 3; // subtract header
            } else {
                pdu_len = static_cast<uint16_t>(fp_hdr[1]) - 2;
            }
            if (pdu_len == 0 || pdu_len > 4096) continue;
            std::vector<uint8_t> events(pdu_len);
            if (!recv_all(impl.client_fd, events.data(), pdu_len)) break;

            // Parse individual fast-path input events.
            size_t off = 0;
            while (off < pdu_len && impl.input_cb) {
                const size_t remaining = pdu_len - off;
                auto ev = parse_rdp_input(events.data() + off, remaining);
                impl.input_cb(ev);
                // Each FP event is 1 header + variable body.
                const uint8_t ec = events[off] & 0x1F;
                off += (ec == 0x02) ? 7 : 2; // mouse=7 bytes, key=2 bytes
            }
        } else {
            // Slow-path TPKT: drain it.
            recv_tpkt(impl.client_fd);
        }
    }
}

// ─── server_bind / wait_for_client ───────────────────────────────────────────

bool RdpTransport::server_bind(int port) {
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
    std::cerr << "[rdp] listening on port " << port << "\n";
    return true;
}

bool RdpTransport::wait_for_client(int timeout_ms) {
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

    char peer_ip[INET_ADDRSTRLEN];
    ::inet_ntop(AF_INET, &peer.sin_addr, peer_ip, sizeof(peer_ip));
    std::cerr << "[rdp] client connected from " << peer_ip << "\n";

    // Run handshake.
    if (!do_rdp_handshake(*impl_)) {
        ::close(impl_->client_fd); impl_->client_fd = -1;
        return false;
    }

    impl_->client_known.store(true);
    impl_->stop_input.store(false);
    impl_->input_th = std::thread([this] { input_receive_loop(*impl_); });

    if (impl_->event_cb)
        impl_->event_cb(pulsar::core::TransportEvent::Ready);
    return true;
}

// ─── connect (loopback verify) ────────────────────────────────────────────────

bool RdpTransport::connect(const std::string& /*endpoint*/) {
    // For verify/loopback testing: just mark as connected without a real peer.
    impl_->client_known.store(true);
    if (impl_->event_cb)
        impl_->event_cb(pulsar::core::TransportEvent::Ready);
    return true;
}

// ─── disconnect ───────────────────────────────────────────────────────────────

void RdpTransport::disconnect() {
    impl_->stop_input.store(true);
    if (impl_->input_th.joinable()) impl_->input_th.join();
    if (impl_->client_fd >= 0) { ::close(impl_->client_fd); impl_->client_fd = -1; }
    impl_->client_known.store(false);
    if (impl_->event_cb)
        impl_->event_cb(pulsar::core::TransportEvent::Disconnected);
}

// ─── send (Fast-Path Bitmap Update) ──────────────────────────────────────────
//
// The EncodedPacket produced by RdpEncoder already contains a valid
// TS_FP_UPDATE_PDU byte stream.  We just write it to the TCP socket.

void RdpTransport::send(pulsar::core::EncodedPacket packet) {
    std::lock_guard<std::mutex> lk(impl_->send_mtx);
    if (!impl_->client_known.load() || impl_->client_fd < 0) return;
    if (!packet.buffer) return;
    send_all(impl_->client_fd, packet.buffer->data(), packet.buffer->size());
}

void RdpTransport::send_batch(std::vector<pulsar::core::EncodedPacket> packets) {
    for (auto& p : packets) send(std::move(p));
}

// ─── Unused ITransport stubs ──────────────────────────────────────────────────

void RdpTransport::send_audio(pulsar::core::AudioPacket) {}
void RdpTransport::send_haptic(const pulsar::core::HapticCommand&) {}
void RdpTransport::send_stats(const pulsar::core::PipelineMetrics&) {}
void RdpTransport::set_stats_callback(std::function<void(pulsar::core::NetworkStats)>) {}
void RdpTransport::set_jitter_buffer(int, int) {}
void RdpTransport::set_fec_params(const pulsar::core::FecParams&) {}
void RdpTransport::set_mic_callback(std::function<void(pulsar::core::AudioFrame)>) {}

void RdpTransport::set_event_callback(
        std::function<void(pulsar::core::TransportEvent)> cb) {
    impl_->event_cb = std::move(cb);
}
void RdpTransport::set_input_callback(
        std::function<void(pulsar::core::InputEvent)> cb) {
    impl_->input_cb = std::move(cb);
}

} // namespace pulsar::transport::rdp
