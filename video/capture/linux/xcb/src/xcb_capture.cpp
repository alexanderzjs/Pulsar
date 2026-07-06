// video/capture/linux/xcb/src/xcb_capture.cpp
// Captures frames from an X11 display using XCB shared memory (zero extra copy).
// Works with any X11 display: Xvfb, Xwayland, Xorg.
// Output format: BGRA (matches DrmVirtualCapture and GPU preprocessor input).

#include "xcb_capture.h"

#include <xcb/xcb.h>
#include <xcb/xcb_image.h>
#include <xcb/shm.h>

#include <chrono>
#include <cstring>
#include <iostream>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <ctime>
#include <thread>

namespace pulsar::capture::xcb {

using namespace std::chrono;

struct XcbCapture::Impl {
    xcb_connection_t*     conn     = nullptr;
    xcb_screen_t*         screen   = nullptr;
    int                   width    = 0;
    int                   height   = 0;
    int                   fps      = 30;
    std::string           display;

    // SHM segment
    xcb_shm_segment_info_t shm_info{};
    int                    shm_id   = -1;
    uint8_t*               shm_data = nullptr;
    size_t                 shm_size = 0;
    bool                   has_shm  = false;

    std::chrono::steady_clock::time_point last_frame = steady_clock::now();

    std::function<void(pulsar::core::CaptureEvent)> event_cb;

    bool open(const std::string& disp) {
        display = disp;
        int screen_num = 0;
        conn = xcb_connect(disp.empty() ? nullptr : disp.c_str(), &screen_num);
        if (!conn || xcb_connection_has_error(conn)) {
            std::cerr << "[xcb] cannot connect to display " << disp << "\n";
            return false;
        }
        const xcb_setup_t* setup = xcb_get_setup(conn);
        auto it = xcb_setup_roots_iterator(setup);
        for (int i = 0; i < screen_num; ++i) xcb_screen_next(&it);
        screen = it.data;
        width  = screen->width_in_pixels;
        height = screen->height_in_pixels;

        // Try SHM extension for zero-copy screenshot
        auto shm_cookie = xcb_shm_query_version(conn);
        auto shm_reply  = xcb_shm_query_version_reply(conn, shm_cookie, nullptr);
        has_shm = (shm_reply != nullptr);
        free(shm_reply);

        if (has_shm) {
            shm_size = (size_t)width * height * 4; // BGRA
            shm_id = shmget(IPC_PRIVATE, shm_size, IPC_CREAT | 0600);
            if (shm_id < 0) { has_shm = false; }
            else {
                shm_data = static_cast<uint8_t*>(shmat(shm_id, nullptr, 0));
                auto seg = xcb_generate_id(conn);
                shm_info.shmid   = shm_id;
                shm_info.shmaddr = shm_data;
                shm_info.shmseg  = seg;
                xcb_shm_attach(conn, seg, shm_id, 0);
            }
        }
        std::cerr << "[xcb] connected to " << disp
                  << " " << width << "x" << height
                  << " shm=" << has_shm << "\n";
        return true;
    }

    ~Impl() {
        if (has_shm && shm_info.shmseg) {
            xcb_shm_detach(conn, shm_info.shmseg);
            shmdt(shm_data);
            shmctl(shm_id, IPC_RMID, nullptr);
        }
        if (conn) xcb_disconnect(conn);
    }
};

XcbCapture::XcbCapture(const std::string& display_name)
    : impl_(std::make_unique<Impl>())
{
    impl_->open(display_name);
}
XcbCapture::~XcbCapture() = default;

bool XcbCapture::is_open() const {
    return impl_->conn && !xcb_connection_has_error(impl_->conn);
}

std::optional<pulsar::core::RawFrame> XcbCapture::next_frame()
{
    auto& m = *impl_;
    if (!m.conn || xcb_connection_has_error(m.conn)) return std::nullopt;

    // Throttle to target FPS
    const auto interval = microseconds(1'000'000 / m.fps);
    const auto now = steady_clock::now();
    if (now - m.last_frame < interval)
        std::this_thread::sleep_for(interval - (now - m.last_frame));
    m.last_frame = steady_clock::now();

    struct BgraBuf final : public pulsar::core::FrameBuffer {
        std::vector<uint8_t> d;
        explicit BgraBuf(size_t sz) : d(sz) {}
        uint8_t* data() const override { return const_cast<uint8_t*>(d.data()); }
        size_t   size() const override { return d.size(); }
    };

    auto buf = std::make_shared<BgraBuf>((size_t)m.width * m.height * 4);

    if (m.has_shm) {
        // Zero-copy: GPU writes directly into SHM
        auto cookie = xcb_shm_get_image(m.conn,
            m.screen->root,
            0, 0, m.width, m.height,
            ~0u,                       // plane mask: all planes
            XCB_IMAGE_FORMAT_Z_PIXMAP,
            m.shm_info.shmseg, 0);
        auto reply = xcb_shm_get_image_reply(m.conn, cookie, nullptr);
        if (!reply) return std::nullopt;
        free(reply);
        std::memcpy(buf->d.data(), m.shm_data, buf->d.size());
    } else {
        // Fallback: copy through server
        auto cookie = xcb_get_image(m.conn,
            XCB_IMAGE_FORMAT_Z_PIXMAP,
            m.screen->root,
            0, 0, m.width, m.height,
            ~0u);
        auto reply = xcb_get_image_reply(m.conn, cookie, nullptr);
        if (!reply) return std::nullopt;
        uint8_t* src = xcb_get_image_data(reply);
        int len = xcb_get_image_data_length(reply);
        if (len > 0)
            std::memcpy(buf->d.data(), src, std::min((size_t)len, buf->d.size()));
        free(reply);
    }

    timespec ts{}; clock_gettime(CLOCK_MONOTONIC, &ts);
    pulsar::core::RawFrame f;
    f.buffer = std::move(buf);
    f.width  = m.width;
    f.height = m.height;
    f.pts_us = (int64_t)ts.tv_sec * 1'000'000 + ts.tv_nsec / 1'000;
    f.format = pulsar::core::PixelFormat::BGRA;
    f.dirty_rects.push_back({0, 0, m.width, m.height});
    return f;
}

std::optional<pulsar::core::CursorState> XcbCapture::next_cursor() {
    return std::nullopt;
}
std::vector<pulsar::core::DisplayInfo> XcbCapture::enumerate_displays() const {
    if (!impl_->conn) return {};
    return {{ 0, impl_->display, impl_->width, impl_->height, impl_->fps, true, false }};
}
void XcbCapture::select_display(int) {}
int  XcbCapture::display_refresh_rate() const { return impl_->fps; }
void XcbCapture::set_event_callback(std::function<void(pulsar::core::CaptureEvent)> cb) {
    impl_->event_cb = std::move(cb);
}
pulsar::core::AdapterCapabilities XcbCapture::capabilities() const {
    pulsar::core::AdapterCapabilities c;
    c.output_formats = { pulsar::core::PixelFormat::BGRA };
    c.color_spaces   = { pulsar::core::ColorSpace::BT709 };
    c.supports_gpu_preprocessing = false; // use CPU BGRA→NV12 (avoid broken VAAPI on this system)
    return c;
}

} // namespace pulsar::capture::xcb
