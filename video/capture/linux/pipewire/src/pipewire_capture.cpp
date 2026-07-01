#include "pipewire_capture.h"

#include <pipewire/pipewire.h>
#include <spa/buffer/buffer.h>
#include <spa/param/video/format-utils.h>
#include <spa/param/video/raw-utils.h>
#include <spa/pod/builder.h>

#include <cstring>
#include <ctime>
#include <iostream>
#include <memory>
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
    ev.version       = PW_VERSION_STREAM_EVENTS;
    ev.process       = PipeWireCapture::on_process;
    ev.state_changed = PipeWireCapture::on_state_changed;
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

// ─── Construction / destruction ────────────────────────────────────────────

PipeWireCapture::PipeWireCapture() {
    pw_init(nullptr, nullptr);

    loop_ = pw_main_loop_new(nullptr);
    if (!loop_) return;

    context_ = pw_context_new(pw_main_loop_get_loop(loop_), nullptr, 0);
    if (!context_) return;

    core_ = pw_context_connect(context_, nullptr, 0);
    if (!core_) return;

    auto* props = pw_properties_new(
        PW_KEY_MEDIA_CLASS, "Video/Source",
        PW_KEY_NODE_NAME,   "Pulsar Capture",
        PW_KEY_MEDIA_ROLE,  "Screen",
        nullptr);

    stream_ = pw_stream_new_simple(
        pw_main_loop_get_loop(loop_),
        "pulsar-capture",
        props,
        &kStreamEvents,
        this);

    if (!stream_) return;

    // Build EnumFormat parameter for NV12 video.
    uint8_t pod_buf[512];
    spa_pod_builder b;
    spa_pod_builder_init(&b, pod_buf, sizeof(pod_buf));

    spa_video_info_raw info{};
    info.format    = SPA_VIDEO_FORMAT_NV12;
    info.size      = SPA_RECTANGLE(static_cast<uint32_t>(kDefaultWidth),
                                   static_cast<uint32_t>(kDefaultHeight));
    info.framerate = SPA_FRACTION(static_cast<uint32_t>(kDefaultFps), 1);
    info.color_range     = SPA_VIDEO_COLOR_RANGE_0_255;
    info.color_matrix    = SPA_VIDEO_COLOR_MATRIX_BT709;
    info.transfer_function = SPA_VIDEO_TRANSFER_SRGB;
    info.color_primaries = SPA_VIDEO_COLOR_PRIMARIES_BT709;

    const spa_pod* params[] = {
        spa_format_video_raw_build(&b, SPA_PARAM_EnumFormat, &info)
    };

    int rc = pw_stream_connect(
        stream_,
        PW_DIRECTION_INPUT,
        PW_ID_ANY,
        static_cast<pw_stream_flags>(
            PW_STREAM_FLAG_AUTOCONNECT |
            PW_STREAM_FLAG_MAP_BUFFERS |
            PW_STREAM_FLAG_RT_PROCESS),
        params, 1);

    if (rc != 0) {
        std::cerr << "[pipewire_capture] pw_stream_connect failed: " << rc << "\n";
        return;
    }

    connected_.store(true);
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
    frame.width  = kDefaultWidth;
    frame.height = kDefaultHeight;
    frame.format = pulsar::core::PixelFormat::NV12;
    frame.color_info.color_space = pulsar::core::ColorSpace::BT709;
    frame.dirty_rects.push_back({ 0, 0, kDefaultWidth, kDefaultHeight });

    const spa_buffer* sbuf = buf->buffer;
    if (sbuf && sbuf->n_datas > 0 && sbuf->datas[0].data) {
        const auto& d = sbuf->datas[0];
        const uint32_t offset = d.chunk ? d.chunk->offset : 0;
        const uint32_t size   = d.chunk ? d.chunk->size   : d.maxsize;
        const bool is_dmabuf  = d.type == SPA_DATA_DmaBuf;
        frame.buffer = std::make_shared<PwFrameBuffer>(
            static_cast<const uint8_t*>(d.data) + offset, size, is_dmabuf);
    } else {
        // Produce a synthetic (silent) frame if no buffer data arrives.
        size_t sz = static_cast<size_t>(kDefaultWidth * kDefaultHeight * 3 / 2);
        frame.buffer = std::make_shared<PwFrameBuffer>(nullptr, sz, false);
    }

    pw_stream_queue_buffer(self->stream_, buf);

    {
        std::lock_guard<std::mutex> lk(self->frame_mutex_);
        self->pending_frame_ = std::move(frame);
    }
    self->frame_cv_.notify_one();
}

void PipeWireCapture::on_state_changed(void* userdata,
                                       pw_stream_state /*old*/,
                                       pw_stream_state state,
                                       const char* error) {
    auto* self = static_cast<PipeWireCapture*>(userdata);
    if (!self) return;

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
