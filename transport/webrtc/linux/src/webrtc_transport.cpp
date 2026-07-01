// transport/webrtc/src/webrtc_transport.cpp
// WebRTC transport with libnice ICE and UDP media delivery.
// Dependency: libnice  [system: pkg-config nice]  — ICE implementation
//
// Flow:
//   1. generate_sdp_offer() — gather local ICE candidates via libnice
//   2. Client returns SDP answer → apply_sdp_answer()
//   3. ICE connectivity check completes → connected_ = true
//   4. send()/send_audio() → UDP datagrams to the ICE-selected path
//   5. Input events arrive on DataChannel → input_cb_ triggered

#include "webrtc_transport.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include <cstring>
#include <iostream>
#include <sstream>
#include <mutex>

// libnice ICE
#include <nice/nice.h>
#include <nice/agent.h>

namespace pulsar::transport::webrtc {

// ─── Impl ─────────────────────────────────────────────────────────────────────
struct WebRtcTransport::Impl {
    GMainLoop*  gloop   = nullptr;
    NiceAgent*  agent   = nullptr;
    guint       stream_id = 0;

    // Fallback UDP socket used before ICE completes.
    int         udp_fd  = -1;
    sockaddr_in peer{};
};

// ─── Construction ─────────────────────────────────────────────────────────────
WebRtcTransport::WebRtcTransport() : impl_(std::make_unique<Impl>()) {}

WebRtcTransport::~WebRtcTransport() { disconnect(); }

// ─── ICE ready callback ───────────────────────────────────────────────────────
static void on_candidate_gathering_done(NiceAgent*, guint, gpointer user_data) {
    auto* self = static_cast<WebRtcTransport*>(user_data);
    (void)self;
    std::cerr << "[webrtc] ICE candidate gathering complete\n";
}

static void on_component_state_changed(NiceAgent*, guint, guint component_id,
                                        guint state, gpointer user_data)
{
    auto* self = static_cast<WebRtcTransport*>(user_data);
    if (component_id == 1 && state == NICE_COMPONENT_STATE_READY) {
        self->connected_.store(true);
        if (self->event_cb_) self->event_cb_(pulsar::core::TransportEvent::Ready);
        std::cerr << "[webrtc] ICE connected\n";
    } else if (state == NICE_COMPONENT_STATE_FAILED) {
        self->connected_.store(false);
        if (self->event_cb_) self->event_cb_(pulsar::core::TransportEvent::Disconnected);
        std::cerr << "[webrtc] ICE failed\n";
    }
}

static void on_receive(NiceAgent*, guint, guint, guint len,
                        gchar* buf, gpointer user_data)
{
    auto* self = static_cast<WebRtcTransport*>(user_data);
    if (!buf || len == 0 || !self->input_cb_) return;
    // Very simple DataChannel input: expect JSON like {"type":"key","code":65}
    // Phase 3: full SCTP DataChannel framing.
    (void)buf; (void)len;  // Parsed by Phase 3 SCTP implementation.
}

// ─── Connect (client mode — not standard for WebRTC, kept for testing) ────────
bool WebRtcTransport::connect(const std::string& endpoint) {
    (void)endpoint;
    // WebRTC uses SDP offer/answer rather than a direct connect endpoint.
    // This method is provided for interface compatibility.
    return generate_sdp_offer() != "";
}

// ─── generate_sdp_offer ────────────────────────────────────────────────────────
std::string WebRtcTransport::generate_sdp_offer() {
    Impl& m = *impl_;

    m.gloop = g_main_loop_new(nullptr, FALSE);
    m.agent = nice_agent_new(g_main_loop_get_context(m.gloop),
                              NICE_COMPATIBILITY_RFC5245);

    g_object_set(G_OBJECT(m.agent),
                 "stun-server", "stun.l.google.com",
                 "stun-server-port", 19302,
                 nullptr);

    g_signal_connect(m.agent, "candidate-gathering-done",
                     G_CALLBACK(on_candidate_gathering_done), this);
    g_signal_connect(m.agent, "component-state-changed",
                     G_CALLBACK(on_component_state_changed), this);

    m.stream_id = nice_agent_add_stream(m.agent, 1 /* n_components */);
    nice_agent_set_stream_name(m.agent, m.stream_id, "video");

    nice_agent_attach_recv(m.agent, m.stream_id, 1,
                            g_main_loop_get_context(m.gloop),
                            on_receive, this);

    nice_agent_gather_candidates(m.agent, m.stream_id);

    // Run the GLib loop briefly to gather candidates.
    // In production, this would run on a background thread.
    GSource* timeout = g_timeout_source_new(2000);
    g_source_set_callback(timeout, [](gpointer d) -> gboolean {
        g_main_loop_quit(static_cast<GMainLoop*>(d)); return G_SOURCE_REMOVE;
    }, m.gloop, nullptr);
    g_source_attach(timeout, g_main_loop_get_context(m.gloop));
    g_main_loop_run(m.gloop);

    // Generate minimal SDP offer.
    gchar* sdp_str = nice_agent_generate_local_sdp(m.agent);
    std::string sdp = sdp_str ? sdp_str : "";
    g_free(sdp_str);

    std::cerr << "[webrtc] SDP offer generated (" << sdp.size() << " bytes)\n";
    return sdp;
}

bool WebRtcTransport::apply_sdp_answer(const std::string& sdp) {
    if (!impl_->agent || sdp.empty()) return false;
    int rc = nice_agent_parse_remote_sdp(impl_->agent, sdp.c_str());
    std::cerr << "[webrtc] apply_sdp_answer rc=" << rc << "\n";
    return rc > 0;
}

// ─── Disconnect ───────────────────────────────────────────────────────────────
void WebRtcTransport::disconnect() {
    connected_.store(false);
    Impl& m = *impl_;
    if (m.agent) { g_object_unref(m.agent); m.agent = nullptr; }
    if (m.gloop) { g_main_loop_unref(m.gloop); m.gloop = nullptr; }
    if (m.udp_fd >= 0) { ::close(m.udp_fd); m.udp_fd = -1; }
    if (event_cb_) event_cb_(pulsar::core::TransportEvent::Disconnected);
}

// ─── Send (via libnice ICE-selected UDP path) ─────────────────────────────────
void WebRtcTransport::send(pulsar::core::EncodedPacket packet) {
    std::lock_guard<std::mutex> lk(send_mtx_);
    if (!connected_.load() || !impl_->agent || !packet.buffer) return;
    nice_agent_send(impl_->agent, impl_->stream_id, 1,
                    static_cast<guint>(packet.buffer->size()),
                    reinterpret_cast<const gchar*>(packet.buffer->data()));
}

void WebRtcTransport::send_batch(std::vector<pulsar::core::EncodedPacket> packets) {
    for (auto& p : packets) send(std::move(p));
}

void WebRtcTransport::send_audio(pulsar::core::AudioPacket packet) {
    std::lock_guard<std::mutex> lk(send_mtx_);
    if (!connected_.load() || !impl_->agent || !packet.data) return;
    nice_agent_send(impl_->agent, impl_->stream_id, 1,
                    static_cast<guint>(packet.size),
                    reinterpret_cast<const gchar*>(packet.data.get()));
}

// ─── Stubs ────────────────────────────────────────────────────────────────────
void WebRtcTransport::set_event_callback(std::function<void(pulsar::core::TransportEvent)> cb) { event_cb_=std::move(cb); }
void WebRtcTransport::set_stats_callback(std::function<void(pulsar::core::NetworkStats)> cb)   { stats_cb_=std::move(cb); }
void WebRtcTransport::set_input_callback(std::function<void(pulsar::core::InputEvent)> cb)     { input_cb_=std::move(cb); }
void WebRtcTransport::set_mic_callback(std::function<void(pulsar::core::AudioFrame)> cb)       { mic_cb_=std::move(cb); }
void WebRtcTransport::set_jitter_buffer(int,int)                                               {}
void WebRtcTransport::set_fec_params(const pulsar::core::FecParams& p){ std::lock_guard<std::mutex> lk(send_mtx_); fec_params_=p; }
void WebRtcTransport::send_haptic(const pulsar::core::HapticCommand&)                          {}
void WebRtcTransport::send_stats(const pulsar::core::PipelineMetrics&)                         {}
std::string WebRtcTransport::sink_id()  const { return "webrtc"; }
void WebRtcTransport::on_packet(pulsar::core::EncodedPacket p) { send(std::move(p)); }
void WebRtcTransport::on_audio(pulsar::core::AudioPacket p)    { send_audio(std::move(p)); }
bool WebRtcTransport::connected() const { return connected_.load(); }

} // namespace pulsar::transport::webrtc
