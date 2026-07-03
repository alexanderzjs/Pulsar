#include "pipewire_capture.h"
#include "mutter_screencast.h"
#include "portal_screencast.h"
#include "kwin_screencast.h"

#include <pipewire/pipewire.h>
#include <spa/buffer/buffer.h>
#include <spa/param/video/format-utils.h>
#include <spa/param/video/raw-utils.h>
#include <spa/pod/builder.h>
#include <systemd/sd-bus.h>

#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <ctime>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace pulsar::capture::pipewire {

namespace {

int64_t now_us() {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<int64_t>(ts.tv_sec) * 1'000'000 + ts.tv_nsec / 1'000;
}

// PipeWire stream event table — must outlive the stream.
pw_stream_events make_stream_events() {
    pw_stream_events ev{};
    ev.version        = PW_VERSION_STREAM_EVENTS;
    ev.process        = PipeWireCapture::on_process;
    ev.state_changed  = PipeWireCapture::on_state_changed;
    ev.param_changed  = PipeWireCapture::on_param_changed;
    return ev;
}

const pw_stream_events kStreamEvents = make_stream_events();

// Minimal FrameBuffer wrapping a heap-copied PipeWire buffer.
struct PwFrameBuffer final : public pulsar::core::FrameBuffer {
    std::vector<uint8_t> data_;
    void*                native_ = nullptr;

    PwFrameBuffer(const void* src, size_t sz, bool is_dmabuf)
        : data_(sz), native_(is_dmabuf ? reinterpret_cast<void*>(1) : nullptr)
    {
        if (src && sz > 0) std::memcpy(data_.data(), src, sz);
    }

    uint8_t* data()          const override { return const_cast<uint8_t*>(data_.data()); }
    size_t   size()          const override { return data_.size(); }
    void*    native_handle() const override { return native_; }
};

} // namespace

// ─── PortalSession ────────────────────────────────────────────────────────

PortalSession::~PortalSession() {
    if (bus) { sd_bus_unref(bus); bus = nullptr; }
}

// ─── Construction / destruction ────────────────────────────────────────────

PipeWireCapture::PipeWireCapture() {
    pw_init(nullptr, nullptr);

    // Priority order — highest to lowest:
    //
    // 1. Mutter ScreenCast via user session bus (no dialog, instant, GNOME only)
    //    Works when the user is logged in to GNOME and Pulsar runs as the same user.
    //    This is exactly how gnome-remote-desktop works — no authorization prompt.
    //
    // 2. Mutter ScreenCast via compositor bus search (root / different UID)
    //    For cases like running as root with sudo, or accessing another session.
    //
    // 3. xdg-desktop-portal (universal, shows dialog once on first use)
    //    Works on any compositor with a portal backend.
    //    On subsequent runs the portal remembers the decision — no repeat dialog.
    //
    // 4. AUTOCONNECT fallback (cameras / virtual sources)

    // ── 1. Mutter via user's own session bus (no dialog) ─────────────────
    uint32_t portal_node_id = PW_ID_ANY;
    sd_bus* portal_bus_tmp  = nullptr;
    {
        sd_bus* bus = nullptr;
        if (sd_bus_open_user(&bus) >= 0) {
            portal_node_id = open_mutter_screencast_user(bus);
            sd_bus_unref(bus);  // Mutter doesn't need the bus kept alive
        }
    }

    // ── 2. Mutter via compositor bus search ──────────────────────────────
    if (portal_node_id == PW_ID_ANY) {
        std::string comp_bus = find_compositor_bus();
        if (!comp_bus.empty())
            portal_node_id = open_mutter_screencast(comp_bus);
    }

    // ── 3. xdg-desktop-portal (shows dialog once) ────────────────────────
    int portal_pw_fd = -1;
    if (portal_node_id == PW_ID_ANY) {
        auto pres = open_portal_screencast();
        if (pres.valid()) {
            portal_node_id  = pres.node_id;
            portal_pw_fd    = pres.pw_fd;
            portal_bus_tmp  = pres.bus;
            pres.bus        = nullptr;  // we took ownership above
        }
    }

    const bool use_portal = (portal_node_id != PW_ID_ANY);

    // ── PipeWire setup ────────────────────────────────────────────────────
    loop_ = pw_main_loop_new(nullptr);
    if (!loop_) return;

    context_ = pw_context_new(pw_main_loop_get_loop(loop_), nullptr, 0);
    if (!context_) return;

    core_ = pw_context_connect(context_, nullptr, 0);
    if (!core_) return;
    // Portal fd was for authentication only — the stream uses the system socket
    // so WirePlumber (session manager) can see and link it to node 73.
    // For sandboxed (Flatpak) use the portal remote would be needed, but for a
    // non-sandboxed server WirePlumber manages the main PipeWire graph.
    if (portal_pw_fd >= 0) { ::close(portal_pw_fd); portal_pw_fd = -1; }

    auto* props = pw_properties_new(
        PW_KEY_MEDIA_TYPE,     "Video",
        PW_KEY_MEDIA_CATEGORY, "Capture",
        PW_KEY_MEDIA_ROLE,     "Screen",
        PW_KEY_NODE_NAME,      "pulsar-capture",
        nullptr);

    // pw_stream_new uses our existing core_ (portal-authenticated when portal
    // fd was used).  pw_stream_new_simple would create a fresh core_ via the
    // default socket and bypass the portal permissions entirely.
    stream_ = pw_stream_new(core_, "pulsar-capture", props);
    // props ownership transferred to pw_stream_new — do NOT free.

    if (!stream_) return;
    pw_stream_add_listener(stream_, &stream_listener_, &kStreamEvents, this);

    // Build EnumFormat parameters.
    // GNOME portals typically provide BGRA; cameras often provide NV12.
    // We advertise both so the compositor can pick what it can supply.
    uint8_t pod_buf[1024];
    spa_pod_builder b;
    spa_pod_builder_init(&b, pod_buf, sizeof(pod_buf));

    auto make_format = [&](spa_video_format fmt) -> const spa_pod* {
        spa_video_info_raw info{};
        info.format    = fmt;
        info.size      = SPA_RECTANGLE(0, 0);   // 0 = any size (portal chooses)
        info.framerate = SPA_FRACTION(0, 1);     // 0 = any rate
        return spa_format_video_raw_build(&b, SPA_PARAM_EnumFormat, &info);
    };

    // Offer BGRA first (portal/compositor preference), then NV12.
    const spa_pod* params[] = {
        make_format(SPA_VIDEO_FORMAT_BGRx),
        make_format(SPA_VIDEO_FORMAT_BGRA),
        make_format(SPA_VIDEO_FORMAT_NV12),
        make_format(SPA_VIDEO_FORMAT_RGBx),
    };

    // Connect to the portal node specifically (if we got one), or fall back
    // to AUTOCONNECT for cameras / virtual sources.
    // PW_STREAM_FLAG_AUTOCONNECT is REQUIRED — without it pw_stream_connect
    // registers the stream with the daemon but never creates the link to the
    // producer node, so the stream stays in PAUSED forever.
    const uint32_t target_id = use_portal ? portal_node_id : PW_ID_ANY;
    const pw_stream_flags flags = static_cast<pw_stream_flags>(
        PW_STREAM_FLAG_AUTOCONNECT |
        PW_STREAM_FLAG_MAP_BUFFERS);

    int rc = pw_stream_connect(stream_, PW_DIRECTION_INPUT, target_id, flags,
                               params, static_cast<uint32_t>(std::size(params)));
    if (rc != 0) {
        std::cerr << "[pipewire_capture] pw_stream_connect failed: " << rc << "\n";
        return;
    }

    if (use_portal) {
        std::cerr << "[pipewire_capture] connected to portal screen (node " << portal_node_id << ")\n";
    } else {
        std::cerr << "[pipewire_capture] connected with AUTOCONNECT (no portal)\n";
    }

    connected_.store(true);
    portal_bus_ = portal_bus_tmp;  // keep D-Bus session alive for portal
    loop_thread_ = std::thread(&PipeWireCapture::event_loop_thread, this);
}

PipeWireCapture::~PipeWireCapture() {
    teardown();
}

void PipeWireCapture::teardown() {
    stop_.store(true);
    {
        std::lock_guard<std::mutex> lk(frame_mutex_);
        pending_frame_.reset();
    }
    frame_cv_.notify_all();

    if (loop_) pw_main_loop_quit(loop_);
    if (loop_thread_.joinable()) loop_thread_.join();

    if (stream_)  { pw_stream_destroy(stream_);   stream_  = nullptr; }
    if (core_)    { pw_core_disconnect(core_);     core_    = nullptr; }
    if (context_) { pw_context_destroy(context_);  context_ = nullptr; }
    if (loop_)    { pw_main_loop_destroy(loop_);   loop_    = nullptr; }
    if (portal_bus_) { sd_bus_unref(portal_bus_); portal_bus_ = nullptr; }

    pw_deinit();
}

void PipeWireCapture::event_loop_thread() {
    if (loop_) pw_main_loop_run(loop_);
}

// ─── ICaptureSource ─────────────────────────────────────────────────────────

std::optional<pulsar::core::RawFrame> PipeWireCapture::next_frame() {
    std::unique_lock<std::mutex> lk(frame_mutex_);
    frame_cv_.wait_for(lk, std::chrono::milliseconds(20),
        [this] { return pending_frame_.has_value() || stop_.load(); });

    if (!pending_frame_.has_value()) return std::nullopt;
    auto frame = std::move(*pending_frame_);
    pending_frame_.reset();
    return frame;
}

std::optional<pulsar::core::CursorState> PipeWireCapture::next_cursor() {
    return pulsar::core::CursorState{ .x = 0, .y = 0, .visible = true };
}

std::vector<pulsar::core::DisplayInfo> PipeWireCapture::enumerate_displays() const {
    return {{ 0, "PipeWire Display", kDefaultWidth, kDefaultHeight, kDefaultFps, true, false }};
}

void PipeWireCapture::select_display(int index) {
    display_index_ = index;
}

int PipeWireCapture::display_refresh_rate() const {
    return kDefaultFps;
}

void PipeWireCapture::set_event_callback(std::function<void(pulsar::core::CaptureEvent)> cb) {
    event_cb_ = std::move(cb);
}

pulsar::core::AdapterCapabilities PipeWireCapture::capabilities() const {
    pulsar::core::AdapterCapabilities caps;
    caps.output_formats    = { pulsar::core::PixelFormat::NV12 };
    caps.color_spaces      = { pulsar::core::ColorSpace::BT709,
                                pulsar::core::ColorSpace::BT2020 };
    caps.supports_dmabuf   = connected_.load();
    caps.supports_headless = true;
    caps.requires_display  = false;
    return caps;
}

// ─── PipeWire callbacks (static) ────────────────────────────────────────────

void PipeWireCapture::on_process(void* userdata) {
    auto* self = static_cast<PipeWireCapture*>(userdata);
    if (!self || self->stop_.load()) return;

    pw_buffer* buf = pw_stream_dequeue_buffer(self->stream_);
    if (!buf) return;

    pulsar::core::RawFrame frame;
    frame.pts_us = now_us();
    frame.width  = self->width_;
    frame.height = self->height_;
    frame.format = self->negotiated_fmt_;
    frame.color_info.color_space = pulsar::core::ColorSpace::BT709;
    frame.dirty_rects.push_back({ 0, 0, frame.width, frame.height });

    const spa_buffer* sbuf = buf->buffer;
    if (sbuf && sbuf->n_datas > 0 && sbuf->datas[0].data) {
        const auto& d = sbuf->datas[0];
        const uint32_t offset = d.chunk ? d.chunk->offset : 0;
        const uint32_t size   = d.chunk ? d.chunk->size   : d.maxsize;
        const bool is_dmabuf  = d.type == SPA_DATA_DmaBuf;
        frame.buffer = std::make_shared<PwFrameBuffer>(
            static_cast<const uint8_t*>(d.data) + offset, size, is_dmabuf);
    } else {
        // No buffer data — skip this frame entirely.
        pw_stream_queue_buffer(self->stream_, buf);
        return;
    }

    pw_stream_queue_buffer(self->stream_, buf);

    {
        std::lock_guard<std::mutex> lk(self->frame_mutex_);
        self->pending_frame_ = std::move(frame);
    }
    self->frame_cv_.notify_one();
}

void PipeWireCapture::on_param_changed(void* userdata, uint32_t id,
                                        const struct spa_pod* param) {
    auto* self = static_cast<PipeWireCapture*>(userdata);
    if (!self) return;

    // Log every call so we can see if this fires at all.
    std::cerr << "[pipewire_capture] on_param_changed id=" << id
              << " param=" << static_cast<const void*>(param) << "\n";

    if (id != SPA_PARAM_Format || !param) return;

    spa_video_info_raw vi{};
    if (spa_format_video_raw_parse(param, &vi) != 0) return;

    // Store negotiated dimensions.
    if (vi.size.width > 0)  self->width_  = static_cast<int>(vi.size.width);
    if (vi.size.height > 0) self->height_ = static_cast<int>(vi.size.height);

    // Map SPA format → our PixelFormat.
    switch (vi.format) {
    case SPA_VIDEO_FORMAT_BGRA:
    case SPA_VIDEO_FORMAT_BGRx:
        self->negotiated_fmt_ = pulsar::core::PixelFormat::BGRA;
        std::cerr << "[pipewire_capture] format=BGRA "
                  << self->width_ << "x" << self->height_ << "\n";
        break;
    case SPA_VIDEO_FORMAT_RGBA:
    case SPA_VIDEO_FORMAT_RGBx:
        self->negotiated_fmt_ = pulsar::core::PixelFormat::RGBA;
        std::cerr << "[pipewire_capture] format=RGBA "
                  << self->width_ << "x" << self->height_ << "\n";
        break;
    default:
        self->negotiated_fmt_ = pulsar::core::PixelFormat::NV12;
        std::cerr << "[pipewire_capture] format=NV12(spa=" << vi.format << ") "
                  << self->width_ << "x" << self->height_ << "\n";
        break;
    }

    // For portal screenshare streams, the compositor (producer) owns buffer
    // allocation.  Calling pw_stream_update_params from the consumer side
    // conflicts with the portal's pre-negotiated DMA-BUF layout and causes
    // "error alloc buffers: Invalid argument".  Simply accept whatever the
    // producer allocates — PW_STREAM_FLAG_MAP_BUFFERS handles mmap for us.
    (void)self;  // suppress unused-variable warning if logging is removed
}

void PipeWireCapture::on_state_changed(void* userdata,
                                       pw_stream_state old_state,
                                       pw_stream_state state,
                                       const char* error) {
    auto* self = static_cast<PipeWireCapture*>(userdata);
    if (!self) return;

    std::cerr << "[pipewire_capture] stream state: "
              << pw_stream_state_as_string(old_state) << " → "
              << pw_stream_state_as_string(state);
    if (error) std::cerr << " (" << error << ")";
    std::cerr << "\n";

    const bool ok = (state == PW_STREAM_STATE_PAUSED ||
                     state == PW_STREAM_STATE_STREAMING);
    const bool err = (state == PW_STREAM_STATE_ERROR || error != nullptr);

    self->connected_.store(ok && !err);

    if (err && self->event_cb_) {
        self->event_cb_(pulsar::core::CaptureEvent::DeviceLost);
    }
    self->frame_cv_.notify_all();
}

} // namespace pulsar::capture::pipewire
