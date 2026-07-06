// video/capture/linux/pipewire/src/pipewire_capture.cpp
//
// Captures a Wayland compositor output via PipeWire ScreenCast.
// Prefer the portal-backed session flow for self-hosted headless mode, with
// the older GNOME-specific D-Bus path kept as a fallback.

#include "pipewire_capture.h"

#include <gio/gio.h>
#include <pipewire/pipewire.h>
#include <spa/buffer/buffer.h>
#include <spa/param/buffers.h>
#include <spa/param/video/format-utils.h>
#include <spa/pod/builder.h>

#include <chrono>
#include <condition_variable>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <queue>
#include <atomic>
#include <thread>

namespace pulsar::capture::pipewire {

using namespace pulsar::core;
using namespace std::chrono;

namespace {

constexpr const char* kPortalBusName = "org.freedesktop.portal.Desktop";
constexpr const char* kPortalObjectPath = "/org/freedesktop/portal/desktop";
constexpr const char* kPortalRemoteDesktopIface = "org.freedesktop.portal.RemoteDesktop";
constexpr const char* kPortalScreenCastIface = "org.freedesktop.portal.ScreenCast";
constexpr const char* kPortalSessionIface = "org.freedesktop.portal.Session";
constexpr const char* kPortalRequestIface = "org.freedesktop.portal.Request";
constexpr guint       kPortalSourceTypeMonitor = 1;
constexpr guint       kPortalCursorModeEmbedded = 2;
constexpr guint       kPortalDeviceTypes = 7;
constexpr guint       kPortalPersistUntilRevoked = 2;
constexpr int         kPortalTimeoutMs = 10000;

struct PortalRequestWaiter {
    GMainLoop* loop = nullptr;
    guint      signal_id = 0;
    guint      timeout_id = 0;
    guint      response = 2;
    GVariant*  results = nullptr;
    bool       done = false;
};

guint64 next_token_suffix() {
    static std::atomic_uint64_t counter{0};
    return ++counter;
}

std::string make_handle_token(const char* prefix) {
    return std::string(prefix) + std::to_string(next_token_suffix());
}

std::string restore_token_path() {
    if (const char* xdg_config_home = std::getenv("XDG_CONFIG_HOME"); xdg_config_home && xdg_config_home[0]) {
        return std::string(xdg_config_home) + "/pulsar/portal_restore_token";
    }
    if (const char* home = std::getenv("HOME"); home && home[0]) {
        return std::string(home) + "/.config/pulsar/portal_restore_token";
    }
    return {};
}

std::string load_restore_token() {
    const std::string path = restore_token_path();
    if (path.empty()) {
        return {};
    }

    std::ifstream in(path);
    if (!in) {
        return {};
    }

    std::string token;
    std::getline(in, token);
    return token;
}

void save_restore_token(const std::string& token) {
    if (token.empty()) {
        return;
    }

    const std::string path = restore_token_path();
    if (path.empty()) {
        return;
    }

    std::error_code ec;
    const auto parent = std::filesystem::path(path).parent_path();
    if (!parent.empty()) {
        std::filesystem::create_directories(parent, ec);
    }

    std::ofstream out(path, std::ios::trunc);
    if (!out) {
        return;
    }
    out << token << '\n';
}

gboolean on_portal_request_timeout(gpointer data) {
    auto* waiter = static_cast<PortalRequestWaiter*>(data);
    if (waiter->loop) {
        g_main_loop_quit(waiter->loop);
    }
    return G_SOURCE_REMOVE;
}

void on_portal_request_response(GDBusConnection*,
                                const gchar*,
                                const gchar*,
                                const gchar*,
                                const gchar*,
                                GVariant* parameters,
                                gpointer user_data) {
    auto* waiter = static_cast<PortalRequestWaiter*>(user_data);
    g_variant_get(parameters, "(u@a{sv})", &waiter->response, &waiter->results);
    waiter->results = g_variant_ref_sink(waiter->results);
    waiter->done = true;
    if (waiter->loop) {
        g_main_loop_quit(waiter->loop);
    }
}

bool wait_for_portal_request(GDBusConnection* bus,
                             const std::string& request_path,
                             guint* response,
                             GVariant** results,
                             GError** error) {
    PortalRequestWaiter waiter;
    g_autoptr(GMainLoop) loop = g_main_loop_new(nullptr, FALSE);
    waiter.loop = loop;

    waiter.signal_id = g_dbus_connection_signal_subscribe(
        bus,
        kPortalBusName,
        kPortalRequestIface,
        "Response",
        request_path.c_str(),
        nullptr,
        G_DBUS_SIGNAL_FLAGS_NONE,
        on_portal_request_response,
        &waiter,
        nullptr);

    waiter.timeout_id = g_timeout_add(kPortalTimeoutMs, on_portal_request_timeout, &waiter);
    g_main_loop_run(loop);

    if (waiter.timeout_id != 0) {
        g_source_remove(waiter.timeout_id);
        waiter.timeout_id = 0;
    }

    g_dbus_connection_signal_unsubscribe(bus, waiter.signal_id);

    if (!waiter.done) {
        g_clear_pointer(&waiter.results, g_variant_unref);
        g_set_error(error,
                    G_IO_ERROR,
                    G_IO_ERROR_TIMED_OUT,
                    "Timed out waiting for portal response from %s",
                    request_path.c_str());
        return false;
    }

    if (response) {
        *response = waiter.response;
    }
    if (results) {
        *results = g_steal_pointer(&waiter.results);
    } else {
        g_clear_pointer(&waiter.results, g_variant_unref);
    }

    return true;
}

bool call_portal_request(GDBusConnection* bus,
                         const char* iface,
                         const char* method,
                         GVariant* args,
                         guint* response,
                         GVariant** results,
                         GError** error) {
    g_autoptr(GVariant) reply = g_dbus_connection_call_sync(
        bus,
        kPortalBusName,
        kPortalObjectPath,
        iface,
        method,
        args,
        G_VARIANT_TYPE("(o)"),
        G_DBUS_CALL_FLAGS_NONE,
        kPortalTimeoutMs,
        nullptr,
        error);
    if (!reply) {
        return false;
    }

    const char* request_path = nullptr;
    g_variant_get(reply, "(&o)", &request_path);
    if (!request_path || !*request_path) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "Portal did not return a request handle");
        return false;
    }

    return wait_for_portal_request(bus, request_path, response, results, error);
}

std::string get_sender_string(GDBusConnection* bus) {
    if (!bus) {
        return "unknown";
    }
    gchar* sender = g_strdup(g_dbus_connection_get_unique_name(bus) + 1);
    if (!sender || !*sender) {
        return "unknown";
    }
    gchar* dot;
    while ((dot = strstr(sender, ".")) != nullptr) {
        *dot = '_';
    }
    std::string out(sender);
    g_free(sender);
    return out;
}

void create_request_path(GDBusConnection* bus, std::string* out_path, std::string* out_token) {
    static std::atomic_uint32_t request_count{0};
    const auto id = ++request_count;
    if (out_token) {
        *out_token = "Sunshine" + std::to_string(id);
    }
    if (out_path) {
        *out_path = "/org/freedesktop/portal/desktop/request/" + get_sender_string(bus) + "/Sunshine" + std::to_string(id);
    }
}

bool parse_start_stream(GVariant* results, uint32_t* node_id, uint32_t* width, uint32_t* height) {
    g_autoptr(GVariantIter) streams_iter = nullptr;
    if (!g_variant_lookup(results, "streams", "a(ua{sv})", &streams_iter) || !streams_iter) {
        return false;
    }

    uint32_t stream_id = 0;
    g_autoptr(GVariant) stream_options = nullptr;
    if (!g_variant_iter_next(streams_iter, "(u@a{sv})", &stream_id, &stream_options)) {
        return false;
    }

    int32_t stream_width = 0;
    int32_t stream_height = 0;
    if (stream_options) {
        g_variant_lookup(stream_options, "size", "(ii)", &stream_width, &stream_height);
    }

    *node_id = stream_id;
    *width = stream_width;
    *height = stream_height;
    return true;
}

std::string extract_restore_token(GVariant* results) {
    if (!results) {
        return {};
    }

    const gchar* token = nullptr;
    if (g_variant_lookup(results, "restore_token", "&s", &token) && token && token[0] != '\0') {
        return token;
    }
    if (g_variant_lookup(results, "restore_token", "s", &token) && token && token[0] != '\0') {
        return token;
    }
    return {};
}

bool extract_session_handle(GVariant* results, std::string* session_path) {
    if (!results || !session_path) {
        return false;
    }

    const gchar* handle = nullptr;
    if (g_variant_lookup(results, "session_handle", "&o", &handle) && handle && *handle) {
        *session_path = handle;
        return true;
    }

    if (g_variant_lookup(results, "session_handle", "&s", &handle) && handle && *handle) {
        *session_path = handle;
        return true;
    }

    g_autoptr(GVariant) session_handle_v = g_variant_lookup_value(results, "session_handle", G_VARIANT_TYPE_VARIANT);
    if (session_handle_v && g_variant_is_of_type(session_handle_v, G_VARIANT_TYPE_VARIANT)) {
        g_autoptr(GVariant) inner = g_variant_get_variant(session_handle_v);
        if (inner && (g_variant_is_of_type(inner, G_VARIANT_TYPE_STRING) ||
                      g_variant_is_of_type(inner, G_VARIANT_TYPE_OBJECT_PATH))) {
            handle = g_variant_get_string(inner, nullptr);
            if (handle && *handle) {
                *session_path = handle;
                return true;
            }
        }
    }
    return false;
}

bool create_portal_session(GDBusConnection* dbus,
                           const char* iface,
                           const std::string& request_token,
                           const std::string& session_token,
                           guint* response,
                           GVariant** results,
                           GError** error) {
    GVariantBuilder options;
    g_variant_builder_init(&options, G_VARIANT_TYPE_VARDICT);
    g_variant_builder_add(&options, "{sv}", "handle_token", g_variant_new_string(request_token.c_str()));
    g_variant_builder_add(&options, "{sv}", "session_handle_token", g_variant_new_string(session_token.c_str()));
    return call_portal_request(dbus,
                               iface,
                               "CreateSession",
                               g_variant_new("(a{sv})", &options),
                               response,
                               results,
                               error);
}

bool select_remote_desktop_devices(GDBusConnection* dbus,
                                   const std::string& session_path,
                                   const std::string& restore_token,
                                   guint* response,
                                   GVariant** results,
                                   GError** error) {
    GVariantBuilder options;
    g_variant_builder_init(&options, G_VARIANT_TYPE_VARDICT);
    const std::string request_token = make_handle_token("request");
    g_variant_builder_add(&options, "{sv}", "handle_token", g_variant_new_string(request_token.c_str()));
    g_variant_builder_add(&options, "{sv}", "types", g_variant_new_uint32(kPortalDeviceTypes));
    g_variant_builder_add(&options, "{sv}", "persist_mode", g_variant_new_uint32(kPortalPersistUntilRevoked));
    if (!restore_token.empty()) {
        g_variant_builder_add(&options, "{sv}", "restore_token", g_variant_new_string(restore_token.c_str()));
    }
    return call_portal_request(dbus,
                               kPortalRemoteDesktopIface,
                               "SelectDevices",
                               g_variant_new("(oa{sv})", session_path.c_str(), &options),
                               response,
                               results,
                               error);
}

bool select_portal_sources(GDBusConnection* dbus,
                           const char* iface,
                           const std::string& session_path,
                           guint types,
                           bool persist,
                           const std::string& restore_token,
                           guint* response,
                           GVariant** results,
                           GError** error) {
    GVariantBuilder options;
    g_variant_builder_init(&options, G_VARIANT_TYPE_VARDICT);
    const std::string request_token = make_handle_token("request");
    g_variant_builder_add(&options, "{sv}", "handle_token", g_variant_new_string(request_token.c_str()));
    g_variant_builder_add(&options, "{sv}", "types", g_variant_new_uint32(types));
    g_variant_builder_add(&options, "{sv}", "cursor_mode", g_variant_new_uint32(kPortalCursorModeEmbedded));
    g_variant_builder_add(&options, "{sv}", "multiple", g_variant_new_boolean(TRUE));
    if (persist) {
        g_variant_builder_add(&options, "{sv}", "persist_mode", g_variant_new_uint32(kPortalPersistUntilRevoked));
        if (!restore_token.empty()) {
            g_variant_builder_add(&options, "{sv}", "restore_token", g_variant_new_string(restore_token.c_str()));
        }
    }
    return call_portal_request(dbus,
                               iface,
                               "SelectSources",
                               g_variant_new("(oa{sv})", session_path.c_str(), &options),
                               response,
                               results,
                               error);
}

bool start_portal_session(GDBusConnection* dbus,
                          const char* iface,
                          const std::string& session_path,
                          guint* response,
                          GVariant** results,
                          GError** error) {
    GVariantBuilder options;
    g_variant_builder_init(&options, G_VARIANT_TYPE_VARDICT);
    const std::string request_token = make_handle_token("request");
    g_variant_builder_add(&options, "{sv}", "handle_token", g_variant_new_string(request_token.c_str()));
    return call_portal_request(dbus,
                               iface,
                               "Start",
                               g_variant_new("(osa{sv})", session_path.c_str(), "", &options),
                               response,
                               results,
                               error);
}

bool open_portal_pipewire_remote(GDBusConnection* bus,
                                 const char* iface,
                                 const std::string& session_path,
                                 int* fd,
                                 GError** error) {
    if (!fd) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "Missing fd output pointer");
        return false;
    }

    g_auto(GVariantBuilder) options_builder = G_VARIANT_BUILDER_INIT(G_VARIANT_TYPE_VARDICT);
    g_autoptr(GUnixFDList) out_fd_list = nullptr;
    g_autoptr(GVariant) reply = g_dbus_connection_call_with_unix_fd_list_sync(
        bus,
        kPortalBusName,
        kPortalObjectPath,
        iface,
        "OpenPipeWireRemote",
        g_variant_new("(oa{sv})", session_path.c_str(), &options_builder),
        G_VARIANT_TYPE("(h)"),
        G_DBUS_CALL_FLAGS_NONE,
        kPortalTimeoutMs,
        nullptr,
        &out_fd_list,
        nullptr,
        error);
    if (!reply) {
        return false;
    }

    int fd_index = -1;
    g_variant_get(reply, "(h)", &fd_index);
    if (fd_index < 0) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "Portal returned an invalid PipeWire fd handle");
        return false;
    }

    *fd = g_unix_fd_list_get(out_fd_list, fd_index, error);
    return *fd >= 0;
}

bool reopen_portal_bus(GDBusConnection** dbus, const std::string& dbus_addr, const std::string& xdg_rt) {
    if (!dbus) {
        return false;
    }
    if (*dbus) {
        g_object_unref(*dbus);
        *dbus = nullptr;
    }
    if (!xdg_rt.empty()) {
        ::setenv("XDG_RUNTIME_DIR", xdg_rt.c_str(), 1);
    }
    if (!dbus_addr.empty()) {
        ::setenv("DBUS_SESSION_BUS_ADDRESS", dbus_addr.c_str(), 1);
    }
    GError* err = nullptr;
    *dbus = g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, &err);
    if (!*dbus) {
        if (err) {
            std::cerr << "[pw_video] reopen dbus: " << err->message << "\n";
            g_error_free(err);
        }
        return false;
    }
    return true;
}

} // namespace

static int64_t now_us() {
    timespec ts{}; clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<int64_t>(ts.tv_sec) * 1'000'000 + ts.tv_nsec / 1'000;
}

struct VecBuf final : public FrameBuffer {
    std::vector<uint8_t> d;
    explicit VecBuf(size_t sz) : d(sz) {}
    uint8_t* data() const override { return const_cast<uint8_t*>(d.data()); }
    size_t   size() const override { return d.size(); }
};

struct PipeWireCapture::Impl {
    // D-Bus (kept alive throughout streaming to keep ScreenCast session open)
    GDBusConnection* dbus    = nullptr;
    std::string      session_path;
    bool             uses_portal_session = false;
    int              portal_pipewire_fd = -1;
    std::string      dbus_address;
    std::string      xdg_runtime_dir;
    std::string      restore_token;

    uint32_t node_id       = SPA_ID_INVALID;

    // PipeWire
    pw_main_loop* pw_loop    = nullptr;
    pw_context*   pw_ctx     = nullptr;
    pw_core*      pw_core_   = nullptr;
    pw_stream*    pw_stream_ = nullptr;
    spa_hook      stream_hook{};
    std::thread   pw_thread;

    spa_video_info_raw video_info{};
    bool               format_ready = false;

    // Frame queue
    std::mutex              q_mtx;
    std::condition_variable q_cv;
    std::queue<RawFrame>    queue;
    static constexpr size_t kMaxQueue = 3;
    bool                    stop_req  = false;

    bool is_open_ = false;
    std::function<void(CaptureEvent)> event_cb;

    bool init(const std::string& dbus_addr, const std::string& xdg_rt);
    bool setup_portal_screencast();
    bool setup_screencast();
    bool start_pipewire();
    void teardown();

    static void on_process(void* data) {
        auto* self = static_cast<Impl*>(data);
        pw_buffer* buf = pw_stream_dequeue_buffer(self->pw_stream_);
        if (!buf) return;

        if (self->format_ready && !self->stop_req) {
            spa_buffer* sbuf = buf->buffer;
            if (sbuf && sbuf->n_datas > 0 && sbuf->datas[0].data) {
                const spa_data& d   = sbuf->datas[0];
                const uint32_t  off = d.chunk ? d.chunk->offset : 0;
                const uint32_t  sz  = d.chunk ? d.chunk->size   : 0;
                const int w = (int)self->video_info.size.width;
                const int h = (int)self->video_info.size.height;

                if (sz > 0 && w > 0 && h > 0) {
                    auto fb = std::make_shared<VecBuf>(sz);
                    std::memcpy(fb->d.data(),
                                (const uint8_t*)d.data + off,
                                std::min((size_t)sz, fb->d.size()));
                    RawFrame f;
                    f.buffer = std::move(fb);
                    f.width  = w; f.height = h;
                    f.pts_us = now_us();
                    f.format = PixelFormat::BGRA;
                    f.dirty_rects.push_back({0, 0, w, h});
                    std::lock_guard<std::mutex> lk(self->q_mtx);
                    if (self->queue.size() < kMaxQueue) {
                        self->queue.push(std::move(f));
                        self->q_cv.notify_one();
                    }
                }
            }
        }
        pw_stream_queue_buffer(self->pw_stream_, buf);
    }

    static void on_state_changed(void* data, pw_stream_state, pw_stream_state state,
                                  const char* error) {
        auto* self = static_cast<Impl*>(data);
        if (state == PW_STREAM_STATE_ERROR) {
            std::cerr << "[pw_video] error: " << (error ? error : "?") << "\n";
            if (self->event_cb) self->event_cb(CaptureEvent::DeviceLost);
        }
        if (state == PW_STREAM_STATE_STREAMING)
            std::cerr << "[pw_video] streaming\n";
    }

    static void on_param_changed(void* data, uint32_t id, const spa_pod* param) {
        if (!param || id != SPA_PARAM_Format) return;
        auto* self = static_cast<Impl*>(data);
        spa_video_info info{};
        if (spa_format_parse(param, &info.media_type, &info.media_subtype) < 0) return;
        if (info.media_type != SPA_MEDIA_TYPE_video) return;
        if (info.media_subtype != SPA_MEDIA_SUBTYPE_raw) return;
        if (spa_format_video_raw_parse(param, &info.info.raw) < 0) return;
        self->video_info   = info.info.raw;
        self->format_ready = true;
        const int w = (int)self->video_info.size.width;
        const int h = (int)self->video_info.size.height;
        std::cerr << "[pw_video] format: " << w << "x" << h
                  << " fmt=" << self->video_info.format << "\n";

        // Negotiate buffer params: request CPU-accessible memory
        // (MemFd or MemPtr) so we can copy frames without GPU mapping.
        const int stride = w * 4;
        const int sz     = stride * h;
        if (sz <= 0) return;

        uint8_t bbuf[1024];
        spa_pod_builder b = SPA_POD_BUILDER_INIT(bbuf, sizeof(bbuf));
        const spa_pod* buf_params[] = {
            (const spa_pod*)spa_pod_builder_add_object(&b,
                SPA_TYPE_OBJECT_ParamBuffers, SPA_PARAM_Buffers,
                SPA_PARAM_BUFFERS_dataType,
                    SPA_POD_CHOICE_FLAGS_Int(
                        (1u << SPA_DATA_MemFd) | (1u << SPA_DATA_MemPtr)),
                SPA_PARAM_BUFFERS_buffers, SPA_POD_CHOICE_RANGE_Int(2, 1, 32),
                SPA_PARAM_BUFFERS_size,    SPA_POD_Int(sz),
                SPA_PARAM_BUFFERS_stride,  SPA_POD_Int(stride),
                SPA_PARAM_BUFFERS_align,   SPA_POD_Int(16)),
        };
        pw_stream_update_params(self->pw_stream_, buf_params, 1);
    }
};

static const pw_stream_events kVideoEvents = {
    .version       = PW_VERSION_STREAM_EVENTS,
    .destroy       = nullptr,
    .state_changed = PipeWireCapture::Impl::on_state_changed,
    .control_info  = nullptr,
    .io_changed    = nullptr,
    .param_changed = PipeWireCapture::Impl::on_param_changed,
    .add_buffer    = nullptr,
    .remove_buffer = nullptr,
    .process       = PipeWireCapture::Impl::on_process,
    .drained       = nullptr,
    .command       = nullptr,
    .trigger_done  = nullptr,
};

bool PipeWireCapture::Impl::setup_portal_screencast() {
    g_autoptr(GError) error = nullptr;
    g_autoptr(GVariant) results = nullptr;
    guint response = 2;

    restore_token = load_restore_token();

    std::string request_token = make_handle_token("request");
    std::string session_token = make_handle_token("session");

    if (!create_portal_session(dbus,
                               kPortalRemoteDesktopIface,
                               request_token,
                               session_token,
                               &response,
                               &results,
                               &error)) {
        if (error) {
            std::cerr << "[pw_video] portal RemoteDesktop.CreateSession: " << error->message << "\n";
        }
        return false;
    }

    if (response != 0 || !results || !extract_session_handle(results, &session_path)) {
        g_autofree gchar* printed = results ? g_variant_print(results, TRUE) : nullptr;
        std::cerr << "[pw_video] portal RemoteDesktop.CreateSession results: "
                  << (printed ? printed : "<null>") << "\n";
        return false;
    }

    uses_portal_session = true;
    response = 2;
    g_clear_pointer(&results, g_variant_unref);

    if (!select_remote_desktop_devices(dbus,
                                       session_path,
                                       restore_token,
                                       &response,
                                       &results,
                                       &error) || response != 0) {
        if (error) {
            std::cerr << "[pw_video] portal RemoteDesktop.SelectDevices: " << error->message << "\n";
        }
        goto screencast_only;
    }
    if (const auto token = extract_restore_token(results); !token.empty()) {
        restore_token = token;
        save_restore_token(restore_token);
    }

    if (!select_portal_sources(dbus,
                               kPortalScreenCastIface,
                               session_path,
                               kPortalSourceTypeMonitor,
                               false,
                               restore_token,
                               &response,
                               &results,
                               &error) || response != 0) {
        if (error) {
            std::cerr << "[pw_video] portal ScreenCast.SelectSources: " << error->message << "\n";
        }
        goto screencast_only;
    }

    g_clear_pointer(&results, g_variant_unref);
    response = 2;

    if (!start_portal_session(dbus,
                              kPortalRemoteDesktopIface,
                              session_path,
                              &response,
                              &results,
                              &error) || response != 0 || !results ||
        !parse_start_stream(results, &node_id, &video_info.size.width, &video_info.size.height)) {
        if (error) {
            std::cerr << "[pw_video] portal RemoteDesktop.Start: " << error->message << "\n";
        }
        goto screencast_only;
    }

    {
        int pw_fd = -1;
        if (!open_portal_pipewire_remote(dbus, kPortalScreenCastIface, session_path, &pw_fd, &error)) {
            if (error) {
                std::cerr << "[pw_video] portal RemoteDesktop.OpenPipeWireRemote: " << error->message << "\n";
            }
            goto screencast_only;
        }
        portal_pipewire_fd = pw_fd;
        if (const auto token = extract_restore_token(results); !token.empty()) {
            restore_token = token;
            save_restore_token(restore_token);
        }
        return true;
    }

    g_clear_error(&error);
    g_clear_pointer(&results, g_variant_unref);

screencast_only:
    session_path.clear();
    uses_portal_session = false;
    response = 2;
    g_clear_error(&error);
    g_clear_pointer(&results, g_variant_unref);

    request_token = make_handle_token("request");
    std::string screencast_session_token = make_handle_token("session");
    if (!create_portal_session(dbus,
                               kPortalScreenCastIface,
                               request_token,
                               screencast_session_token,
                               &response,
                               &results,
                               &error)) {
        if (error) {
            std::cerr << "[pw_video] portal ScreenCast.CreateSession: " << error->message << "\n";
        }
        return false;
    }

    if (response != 0 || !results || !extract_session_handle(results, &session_path)) {
        g_autofree gchar* printed = results ? g_variant_print(results, TRUE) : nullptr;
        std::cerr << "[pw_video] portal ScreenCast.CreateSession results: "
                  << (printed ? printed : "<null>") << "\n";
        return false;
    }

    uses_portal_session = true;
    response = 2;
    g_clear_pointer(&results, g_variant_unref);

    if (!select_portal_sources(dbus,
                               kPortalScreenCastIface,
                               session_path,
                               1,
                               true,
                               restore_token,
                               &response,
                               &results,
                               &error)) {
        if (error) {
            std::cerr << "[pw_video] portal ScreenCast.SelectSources: " << error->message << "\n";
        }
        return false;
    }

    if (response != 0) {
        std::cerr << "[pw_video] portal ScreenCast.SelectSources rejected with response=" << response << "\n";
        return false;
    }

    response = 2;
    g_clear_pointer(&results, g_variant_unref);

    if (!start_portal_session(dbus,
                              kPortalScreenCastIface,
                              session_path,
                              &response,
                              &results,
                              &error)) {
        if (error) {
            std::cerr << "[pw_video] portal ScreenCast.Start: " << error->message << "\n";
        }
        return false;
    }

    if (response != 0 || !results) {
        std::cerr << "[pw_video] portal ScreenCast.Start rejected with response=" << response << "\n";
        return false;
    }

    if (!parse_start_stream(results, &node_id, &video_info.size.width, &video_info.size.height)) {
        std::cerr << "[pw_video] portal ScreenCast.Start returned no usable stream metadata\n";
        return false;
    }

    int pw_fd = -1;
    if (!open_portal_pipewire_remote(dbus, kPortalScreenCastIface, session_path, &pw_fd, &error)) {
        if (error) {
            std::cerr << "[pw_video] OpenPipeWireRemote: " << error->message << "\n";
        }
        return false;
    }

    portal_pipewire_fd = pw_fd;
    if (const auto token = extract_restore_token(results); !token.empty()) {
        restore_token = token;
        save_restore_token(restore_token);
    }
    return true;
}

bool PipeWireCapture::Impl::setup_screencast() {
    node_id = SPA_ID_INVALID;
    if (!setup_portal_screencast()) {
        std::cerr << "[pw_video] portal ScreenCast init failed\n";
        return false;
    }
    return true;
}

bool PipeWireCapture::Impl::start_pipewire() {
    pw_init(nullptr, nullptr);
    pw_loop = pw_main_loop_new(nullptr);
    if (!pw_loop) return false;
    pw_ctx = pw_context_new(pw_main_loop_get_loop(pw_loop), nullptr, 0);
    if (!pw_ctx) return false;

    if (portal_pipewire_fd >= 0) {
        pw_core_ = pw_context_connect_fd(pw_ctx, portal_pipewire_fd, nullptr, 0);
        portal_pipewire_fd = -1;
    } else {
        pw_core_ = pw_context_connect(pw_ctx, nullptr, 0);
    }
    if (!pw_core_) { std::cerr << "[pw_video] pw_context_connect failed\n"; return false; }

    auto* props = pw_properties_new(
        PW_KEY_MEDIA_TYPE, "Video",
        PW_KEY_MEDIA_CATEGORY, "Capture",
        PW_KEY_MEDIA_ROLE, "Screen",
        nullptr);
    pw_stream_ = pw_stream_new(pw_core_, "pulsar-screencast", props);
    if (!pw_stream_) return false;
    pw_stream_add_listener(pw_stream_, &stream_hook, &kVideoEvents, this);

    // Offer BGRA and RGBx formats — Mutter picks one during negotiation.
    // size/framerate = 0 means "accept whatever the server proposes".
    uint8_t fmtbuf[1024];
    spa_pod_builder fb = SPA_POD_BUILDER_INIT(fmtbuf, sizeof(fmtbuf));
    spa_video_info_raw raw_bgra{};
    raw_bgra.format = SPA_VIDEO_FORMAT_BGRA;
    spa_video_info_raw raw_rgbx{};
    raw_rgbx.format = SPA_VIDEO_FORMAT_RGBx;
    const spa_pod* fmt_params[] = {
        spa_format_video_raw_build(&fb, SPA_PARAM_EnumFormat, &raw_bgra),
        spa_format_video_raw_build(&fb, SPA_PARAM_EnumFormat, &raw_rgbx),
    };

    int rc = pw_stream_connect(pw_stream_, PW_DIRECTION_INPUT, node_id,
        (pw_stream_flags)(PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS),
        fmt_params, 2);
    if (rc != 0) { std::cerr << "[pw_video] connect failed: " << rc << "\n"; return false; }

    pw_thread = std::thread([this] { pw_main_loop_run(pw_loop); });
    std::cerr << "[pw_video] PipeWire connected\n";
    return true;
}

bool PipeWireCapture::Impl::init(const std::string& dbus_addr,
                                   const std::string& xdg_rt) {
    dbus_address = dbus_addr;
    xdg_runtime_dir = xdg_rt;
    if (!xdg_rt.empty())    ::setenv("XDG_RUNTIME_DIR",         xdg_rt.c_str(),    1);
    if (!dbus_addr.empty()) ::setenv("DBUS_SESSION_BUS_ADDRESS", dbus_addr.c_str(), 1);

    // Connect to session bus using current thread's default context.
    // The signal subscription below also uses the default context, so
    // g_main_context_iteration() in setup_screencast() will dispatch it.
    GError* err = nullptr;
    dbus = g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, &err);
    if (!dbus || err) {
        if (err) { std::cerr << "[pw_video] dbus: " << err->message << "\n"; g_error_free(err); }
        return false;
    }

    if (!setup_screencast()) return false;
    return start_pipewire();
}

void PipeWireCapture::Impl::teardown() {
    { std::lock_guard<std::mutex> lk(q_mtx); stop_req = true; }
    q_cv.notify_all();
    if (pw_loop) pw_main_loop_quit(pw_loop);
    if (pw_thread.joinable()) pw_thread.join();
    if (pw_stream_) { pw_stream_destroy(pw_stream_); pw_stream_ = nullptr; }
    if (pw_core_)   { pw_core_disconnect(pw_core_);  pw_core_   = nullptr; }
    if (pw_ctx)     { pw_context_destroy(pw_ctx);    pw_ctx     = nullptr; }
    if (pw_loop)    { pw_main_loop_destroy(pw_loop); pw_loop    = nullptr; }
    // pw_deinit() intentionally omitted: keep PW global state for reconnects.

    if (portal_pipewire_fd >= 0) {
        close(portal_pipewire_fd);
        portal_pipewire_fd = -1;
    }

    // Explicitly close whichever session we opened so the compositor can
    // release any virtual desktop resources before the next reconnect.
    if (dbus && !session_path.empty()) {
        g_dbus_connection_call_sync(
            dbus,
            kPortalBusName,
            session_path.c_str(),
            kPortalSessionIface,
            "Close",
            nullptr, nullptr,
            G_DBUS_CALL_FLAGS_NONE, 2000, nullptr, nullptr);
        session_path.clear();
    }

    if (dbus) { g_object_unref(dbus); dbus = nullptr; }
}

// ─── Public API ───────────────────────────────────────────────────────────────
PipeWireCapture::PipeWireCapture(const std::string& dbus_addr,
                                   const std::string& xdg_rt)
    : impl_(std::make_unique<Impl>())
{
    impl_->is_open_ = impl_->init(dbus_addr, xdg_rt);
    if (!impl_->is_open_)
        std::cerr << "[pw_video] ScreenCast init failed\n";
}
PipeWireCapture::~PipeWireCapture() { if (impl_) impl_->teardown(); }
bool PipeWireCapture::is_open() const { return impl_ && impl_->is_open_; }

void PipeWireCapture::flush_queue() {
    if (!impl_) return;
    std::lock_guard<std::mutex> lk(impl_->q_mtx);
    while (!impl_->queue.empty()) impl_->queue.pop();
}

std::optional<RawFrame> PipeWireCapture::next_frame() {
    if (!impl_ || !impl_->is_open_) return std::nullopt;
    auto& m = *impl_;
    std::unique_lock<std::mutex> lk(m.q_mtx);
    m.q_cv.wait_for(lk, milliseconds(33),
        [&m] { return !m.queue.empty() || m.stop_req; });
    if (m.queue.empty()) return std::nullopt;
    auto f = std::move(m.queue.front()); m.queue.pop();
    return f;
}
std::optional<CursorState> PipeWireCapture::next_cursor() { return std::nullopt; }
std::vector<DisplayInfo> PipeWireCapture::enumerate_displays() const {
    if (!impl_ || !impl_->is_open_) return {};
    const int w = (int)impl_->video_info.size.width;
    const int h = (int)impl_->video_info.size.height;
    return {{ 0, "pipewire-screencast", w ? w : 1920, h ? h : 1080, 30, true, false }};
}
void PipeWireCapture::select_display(int) {}
int  PipeWireCapture::display_refresh_rate() const { return 30; }
void PipeWireCapture::set_event_callback(std::function<void(CaptureEvent)> cb) {
    if (impl_) impl_->event_cb = std::move(cb);
}
AdapterCapabilities PipeWireCapture::capabilities() const {
    AdapterCapabilities c;
    c.output_formats             = { PixelFormat::BGRA };
    c.color_spaces               = { ColorSpace::BT709 };
    c.supports_gpu_preprocessing = false;
    return c;
}

} // namespace pulsar::capture::pipewire
