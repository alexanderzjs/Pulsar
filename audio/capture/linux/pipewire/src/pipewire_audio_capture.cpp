#include "pipewire_audio_capture.h"

#include <pipewire/pipewire.h>
#include <spa/buffer/buffer.h>
#include <spa/param/audio/format-utils.h>
#include <spa/pod/builder.h>

#include <cstring>
#include <ctime>
#include <iostream>

namespace pulsar::audio::capture::pipewire {

static constexpr int kSampleRate   = 48000;
static constexpr int kChannels     = 2;

static int64_t now_us_a() {
    timespec ts{}; clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<int64_t>(ts.tv_sec)*1'000'000 + ts.tv_nsec/1'000;
}

// ─── PipeWire stream events ──────────────────────────────────────────────────
void PipeWireAudioCapture::on_process(void* data) {
    auto* self = static_cast<PipeWireAudioCapture*>(data);
    if (!self || self->stop_req_) return;

    pw_buffer* buf = pw_stream_dequeue_buffer(self->stream_);
    if (!buf) return;

    const spa_buffer* sbuf = buf->buffer;
    if (sbuf && sbuf->n_datas > 0 && sbuf->datas[0].data) {
        const auto& d = sbuf->datas[0];
        const uint32_t offset = d.chunk ? d.chunk->offset : 0;
        const uint32_t size   = d.chunk ? d.chunk->size   : d.maxsize;

        if (size > 0) {
            pulsar::core::AudioFrame frame;
            frame.sample_rate = kSampleRate;
            frame.channels    = kChannels;
            frame.samples     = static_cast<int>(size) / (kChannels * 2); // S16 = 2 bytes
            frame.size        = static_cast<size_t>(size);
            frame.pts_us      = now_us_a();
            frame.format      = pulsar::core::AudioFormat::PCM_S16LE;
            frame.data        = std::shared_ptr<uint8_t[]>(new uint8_t[size]);
            std::memcpy(frame.data.get(),
                        static_cast<const uint8_t*>(d.data) + offset, size);

            std::lock_guard<std::mutex> lk(self->q_mtx_);
            if (self->queue_.size() < kMaxQueueDepth)
                self->queue_.push(std::move(frame));
        }
    }
    pw_stream_queue_buffer(self->stream_, buf);
    self->q_cv_.notify_one();
}

void PipeWireAudioCapture::on_state_changed(void* data,
                                             enum pw_stream_state /*old*/,
                                             enum pw_stream_state state,
                                             const char* error)
{
    auto* self = static_cast<PipeWireAudioCapture*>(data);
    if (!self) return;
    self->connected_ = (state == PW_STREAM_STATE_PAUSED ||
                        state == PW_STREAM_STATE_STREAMING);
    if ((state == PW_STREAM_STATE_ERROR || error) && self->event_cb_)
        self->event_cb_(pulsar::core::AudioCaptureEvent::DeviceLost);
}

// ─── Stream events table ─────────────────────────────────────────────────────
static const pw_stream_events kAudioEvents = {
    .version       = PW_VERSION_STREAM_EVENTS,
    .destroy       = nullptr,
    .state_changed = PipeWireAudioCapture::on_state_changed,
    .control_info  = nullptr,
    .io_changed    = nullptr,
    .param_changed = nullptr,
    .add_buffer    = nullptr,
    .remove_buffer = nullptr,
    .process       = PipeWireAudioCapture::on_process,
    .drained       = nullptr,
    .command       = nullptr,
    .trigger_done  = nullptr,
};

// ─── Construction ─────────────────────────────────────────────────────────────
PipeWireAudioCapture::PipeWireAudioCapture(int /*frame_budget*/) {
    pw_init(nullptr, nullptr);

    loop_ = pw_main_loop_new(nullptr);
    if (!loop_) { std::cerr << "[pw_audio] no main loop\n"; return; }

    ctx_ = pw_context_new(pw_main_loop_get_loop(loop_), nullptr, 0);
    if (!ctx_) { teardown(); return; }

    core_ = pw_context_connect(ctx_, nullptr, 0);
    if (!core_) { teardown(); return; }

    auto* props = pw_properties_new(
        PW_KEY_MEDIA_CLASS, "Audio/Source",
        PW_KEY_NODE_NAME,   "Pulsar Audio Capture",
        PW_KEY_MEDIA_ROLE,  "Screen",
        nullptr);
    stream_ = pw_stream_new_simple(pw_main_loop_get_loop(loop_),
                                   "pulsar-audio", props, &kAudioEvents, this);
    if (!stream_) { teardown(); return; }

    // Negotiate audio format
    uint8_t pod_buf[512];
    spa_pod_builder b; spa_pod_builder_init(&b, pod_buf, sizeof(pod_buf));
    spa_audio_info_raw info{};
    info.format   = SPA_AUDIO_FORMAT_S16_LE;
    info.channels = static_cast<uint32_t>(kChannels);
    info.rate     = static_cast<uint32_t>(kSampleRate);
    const spa_pod* params[] = { spa_format_audio_raw_build(&b, SPA_PARAM_EnumFormat, &info) };

    int rc = pw_stream_connect(stream_, PW_DIRECTION_INPUT, PW_ID_ANY,
                               static_cast<enum pw_stream_flags>(
                                   PW_STREAM_FLAG_AUTOCONNECT |
                                   PW_STREAM_FLAG_MAP_BUFFERS |
                                   PW_STREAM_FLAG_RT_PROCESS),
                               params, 1);
    if (rc != 0) {
        std::cerr << "[pw_audio] pw_stream_connect failed: " << rc << "\n";
        teardown(); return;
    }

    loop_thread_ = std::thread([this] { pw_main_loop_run(loop_); });
    std::cerr << "[pw_audio] capturing system audio (PCM_S16LE 48kHz stereo)\n";
}

PipeWireAudioCapture::~PipeWireAudioCapture() { teardown(); }

void PipeWireAudioCapture::teardown() {
    { std::lock_guard<std::mutex> lk(q_mtx_); stop_req_ = true; }
    q_cv_.notify_all();
    if (loop_)   pw_main_loop_quit(loop_);
    if (loop_thread_.joinable()) loop_thread_.join();
    if (stream_) { pw_stream_destroy(stream_);  stream_ = nullptr; }
    if (core_)   { pw_core_disconnect(core_);   core_   = nullptr; }
    if (ctx_)    { pw_context_destroy(ctx_);    ctx_    = nullptr; }
    if (loop_)   { pw_main_loop_destroy(loop_); loop_   = nullptr; }
    pw_deinit();
}

// ─── IAudioCapture ────────────────────────────────────────────────────────────
std::optional<pulsar::core::AudioFrame> PipeWireAudioCapture::next_frame() {
    if (!stream_) return std::nullopt;
    std::unique_lock<std::mutex> lk(q_mtx_);
    q_cv_.wait_for(lk, std::chrono::milliseconds(20),
        [this] { return !queue_.empty() || stop_req_; });
    if (queue_.empty()) return std::nullopt;
    auto f = std::move(queue_.front());
    queue_.pop();
    return f;
}

void PipeWireAudioCapture::set_event_callback(
    std::function<void(pulsar::core::AudioCaptureEvent)> cb) {
    event_cb_ = std::move(cb);
}

pulsar::core::AdapterCapabilities PipeWireAudioCapture::capabilities() const {
    return {};
}

} // namespace pulsar::audio::capture::pipewire
