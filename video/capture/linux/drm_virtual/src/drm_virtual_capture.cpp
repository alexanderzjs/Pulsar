// capture/drm_virtual/src/drm_virtual_capture.cpp
// DRM/KMS Virtual Display Capture implementation.
//
// Architecture:
//   1. Scan /dev/dri/card* for a VKMS driver device.
//   2. Enumerate connectors; find one of type VIRTUAL (VKMS) or WRITEBACK.
//   3. Pick a mode (preferred or configured resolution).
//   4. Create a DRM "dumb buffer" (software-backed framebuffer, XRGB8888).
//   5. Add framebuffer, set CRTC → virtual connector is now active.
//   6. mmap the dumb buffer.  A Wayland/X11 compositor that has been told
//      to use this KMS device will render into it; we read at the target FPS.
//   7. Convert XRGB8888 → NV12 (BT.601 full-range) and return as RawFrame.
//
// Frame deduplication: XOR-based 64-bit checksum avoids returning identical
// frames when nothing is rendering (saves encode cycles).
//
// Dependency: libdrm (system, pkg-config libdrm).

#include "drm_virtual_capture.h"

#include <xf86drm.h>
#include <xf86drmMode.h>
#include <drm/drm.h>
#include <drm/drm_mode.h>

#include <cerrno>
#include <chrono>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <glob.h>
#include <iostream>
#include <sys/mman.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace pulsar::capture::drm_virtual {

using namespace std::chrono;

// ─── Impl ─────────────────────────────────────────────────────────────────────

struct DrmVirtualCaptureImpl {
    int  drm_fd      = -1;
    int  width       = 1920;
    int  height      = 1080;
    int  fps         = 60;
    bool ready       = false;

    // DRM objects
    uint32_t connector_id = 0;
    uint32_t crtc_id      = 0;
    uint32_t fb_id        = 0;
    uint32_t dumb_handle  = 0;
    uint32_t dumb_pitch   = 0;
    uint64_t dumb_size    = 0;
    void*    fb_map       = MAP_FAILED;

    // Saved old CRTC for restore on teardown
    drmModeCrtc* saved_crtc = nullptr;

    drmModeModeInfo mode_info{};

    // Frame deduplication
    uint64_t last_checksum = ~0ULL;

    std::chrono::steady_clock::time_point last_frame_tp =
        std::chrono::steady_clock::now();

    std::function<void(pulsar::core::CaptureEvent)> event_cb;

    ~DrmVirtualCaptureImpl() { cleanup(); }

    void cleanup() {
        if (fb_map != MAP_FAILED && dumb_size > 0) {
            ::munmap(fb_map, dumb_size);
            fb_map = MAP_FAILED;
        }
        if (drm_fd >= 0) {
            if (saved_crtc) {
                drmModeSetCrtc(drm_fd, saved_crtc->crtc_id,
                               saved_crtc->buffer_id,
                               saved_crtc->x, saved_crtc->y,
                               &connector_id, 1, &saved_crtc->mode);
                drmModeFreeCrtc(saved_crtc);
                saved_crtc = nullptr;
            }
            if (fb_id)          { drmModeRmFB(drm_fd, fb_id); fb_id = 0; }
            if (dumb_handle) {
                drm_mode_destroy_dumb dd{};
                dd.handle = dumb_handle;
                drmIoctl(drm_fd, DRM_IOCTL_MODE_DESTROY_DUMB, &dd);
                dumb_handle = 0;
            }
            ::close(drm_fd);
            drm_fd = -1;
        }
        ready = false;
    }
};

// ─── XRGB8888 → NV12 (BT.601 full-range) ────────────────────────────────────
//
// DRM dumb buffers use XRGB8888 (little-endian: B G R X per 4 bytes).
// The pipeline expects NV12.

static void xrgb_to_nv12(const uint32_t* xrgb, int W, int H,
                           uint8_t* Y_plane, uint8_t* UV_plane) {
    // Luma pass
    for (int row = 0; row < H; ++row) {
        const uint32_t* src = xrgb + row * W;
        uint8_t*        dst = Y_plane + row * W;
        for (int col = 0; col < W; ++col) {
            const uint32_t px = src[col];
            const int b = static_cast<int>((px)       & 0xFF);
            const int g = static_cast<int>((px >>  8) & 0xFF);
            const int r = static_cast<int>((px >> 16) & 0xFF);
            // BT.601 full-range: Y = 0.299R + 0.587G + 0.114B
            dst[col] = static_cast<uint8_t>(
                (77 * r + 150 * g + 29 * b) >> 8);
        }
    }
    // Chroma pass (4:2:0 — average 2×2 blocks)
    for (int row = 0; row < H / 2; ++row) {
        const uint32_t* src0 = xrgb + (row * 2)     * W;
        const uint32_t* src1 = xrgb + (row * 2 + 1) * W;
        uint8_t*        dst  = UV_plane + row * W;
        for (int col = 0; col < W / 2; ++col) {
            auto avgB = [](uint32_t a, uint32_t b, uint32_t c, uint32_t d) {
                return ((a & 0xFF) + (b & 0xFF) + (c & 0xFF) + (d & 0xFF)) >> 2;
            };
            auto avgG = [](uint32_t a, uint32_t b, uint32_t c, uint32_t d) {
                return (((a>>8)&0xFF)+((b>>8)&0xFF)+((c>>8)&0xFF)+((d>>8)&0xFF))>>2;
            };
            auto avgR = [](uint32_t a, uint32_t b, uint32_t c, uint32_t d) {
                return (((a>>16)&0xFF)+((b>>16)&0xFF)+((c>>16)&0xFF)+((d>>16)&0xFF))>>2;
            };
            const uint32_t p00 = src0[col*2], p01 = src0[col*2+1];
            const uint32_t p10 = src1[col*2], p11 = src1[col*2+1];
            const int r = static_cast<int>(avgR(p00,p01,p10,p11));
            const int g = static_cast<int>(avgG(p00,p01,p10,p11));
            const int b = static_cast<int>(avgB(p00,p01,p10,p11));
            // BT.601 full-range Cb = -0.169R - 0.331G + 0.500B + 128
            dst[col*2]   = static_cast<uint8_t>(
                std::clamp((-43*r - 85*g + 128*b) / 256 + 128, 0, 255));
            // Cr = 0.500R - 0.419G - 0.081B + 128
            dst[col*2+1] = static_cast<uint8_t>(
                std::clamp((128*r - 107*g - 21*b) / 256 + 128, 0, 255));
        }
    }
}

// ─── Device scan ─────────────────────────────────────────────────────────────

// Scan /dev/dri/card* and return the first device whose driver is "vkms".
static std::string find_vkms_device() {
    glob_t gl{};
    if (glob("/dev/dri/card*", 0, nullptr, &gl) != 0) return {};
    std::string result;
    for (size_t i = 0; i < gl.gl_pathc; ++i) {
        int fd = ::open(gl.gl_pathv[i], O_RDWR | O_CLOEXEC);
        if (fd < 0) continue;
        drmVersion* ver = drmGetVersion(fd);
        if (ver) {
            if (std::string(ver->name) == "vkms") {
                result = gl.gl_pathv[i];
                drmFreeVersion(ver);
                ::close(fd);
                break;
            }
            drmFreeVersion(ver);
        }
        ::close(fd);
    }
    globfree(&gl);
    return result;
}

// ─── DrmVirtualCapture public API ────────────────────────────────────────────

DrmVirtualCapture::DrmVirtualCapture()
    : impl_(std::make_unique<DrmVirtualCaptureImpl>()) {}

DrmVirtualCapture::~DrmVirtualCapture() = default;

void DrmVirtualCapture::set_resolution(int width, int height, int fps) {
    impl_->width  = width;
    impl_->height = height;
    impl_->fps    = fps;
}

bool DrmVirtualCapture::is_available() {
    return !find_vkms_device().empty();
}

// ─── open_vkms_device ────────────────────────────────────────────────────────

bool DrmVirtualCapture::open_vkms_device() {
    const std::string dev = find_vkms_device();
    if (dev.empty()) {
        std::cerr << "[drm_virtual] no VKMS device found "
                     "(run: sudo modprobe vkms)\n";
        return false;
    }
    impl_->drm_fd = ::open(dev.c_str(), O_RDWR | O_CLOEXEC);
    if (impl_->drm_fd < 0) {
        std::cerr << "[drm_virtual] open " << dev
                  << " failed: " << std::strerror(errno) << "\n";
        return false;
    }
    drmSetClientCap(impl_->drm_fd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1);
    std::cerr << "[drm_virtual] opened VKMS device: " << dev << "\n";
    return true;
}

// ─── setup_virtual_display ───────────────────────────────────────────────────

bool DrmVirtualCapture::setup_virtual_display() {
    auto& m = *impl_;

    drmModeRes* res = drmModeGetResources(m.drm_fd);
    if (!res) {
        std::cerr << "[drm_virtual] drmModeGetResources failed\n";
        return false;
    }

    // Find first connector of type VIRTUAL (VKMS) or any disconnected/unknown.
    drmModeConnector* conn = nullptr;
    for (int i = 0; i < res->count_connectors && !conn; ++i) {
        drmModeConnector* c = drmModeGetConnector(m.drm_fd, res->connectors[i]);
        if (!c) continue;
        if (c->connector_type == DRM_MODE_CONNECTOR_VIRTUAL ||
            c->connector_type == DRM_MODE_CONNECTOR_WRITEBACK ||
            c->connection != DRM_MODE_DISCONNECTED) {
            conn = c;
            m.connector_id = c->connector_id;
        } else {
            drmModeFreeConnector(c);
        }
    }
    if (!conn) {
        std::cerr << "[drm_virtual] no usable connector found\n";
        drmModeFreeResources(res);
        return false;
    }

    // Pick a mode: prefer exact W×H match; otherwise take first or add custom.
    bool mode_found = false;
    for (int i = 0; i < conn->count_modes; ++i) {
        if (static_cast<int>(conn->modes[i].hdisplay) == m.width &&
            static_cast<int>(conn->modes[i].vdisplay) == m.height) {
            m.mode_info  = conn->modes[i];
            mode_found   = true;
            break;
        }
    }
    if (!mode_found && conn->count_modes > 0) {
        m.mode_info  = conn->modes[0];
        m.width      = static_cast<int>(m.mode_info.hdisplay);
        m.height     = static_cast<int>(m.mode_info.vdisplay);
        m.fps        = static_cast<int>(m.mode_info.vrefresh);
        mode_found   = true;
    }
    if (!mode_found) {
        // VKMS supports adding custom modes via drmModeAttachMode.
        // Build a simple 1920×1080@60 CVT mode info manually.
        std::memset(&m.mode_info, 0, sizeof(m.mode_info));
        m.mode_info.clock       = 148500;
        m.mode_info.hdisplay    = static_cast<uint16_t>(m.width);
        m.mode_info.hsync_start = static_cast<uint16_t>(m.width + 88);
        m.mode_info.hsync_end   = static_cast<uint16_t>(m.width + 88 + 44);
        m.mode_info.htotal      = static_cast<uint16_t>(m.width + 280);
        m.mode_info.vdisplay    = static_cast<uint16_t>(m.height);
        m.mode_info.vsync_start = static_cast<uint16_t>(m.height + 4);
        m.mode_info.vsync_end   = static_cast<uint16_t>(m.height + 4 + 5);
        m.mode_info.vtotal      = static_cast<uint16_t>(m.height + 45);
        m.mode_info.vrefresh    = static_cast<uint32_t>(m.fps);
        m.mode_info.flags       = DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC;
        m.mode_info.type        = DRM_MODE_TYPE_DRIVER | DRM_MODE_TYPE_PREFERRED;
        std::snprintf(m.mode_info.name, DRM_DISPLAY_MODE_LEN,
                      "%dx%d", m.width, m.height);
        drmModeAttachMode(m.drm_fd, m.connector_id, &m.mode_info);
    }

    // Find an encoder + CRTC for this connector.
    uint32_t crtc_id = 0;
    for (int ei = 0; ei < conn->count_encoders && !crtc_id; ++ei) {
        drmModeEncoder* enc =
            drmModeGetEncoder(m.drm_fd, conn->encoders[ei]);
        if (!enc) continue;
        for (int ci = 0; ci < res->count_crtcs && !crtc_id; ++ci) {
            if (enc->possible_crtcs & (1u << ci))
                crtc_id = res->crtcs[ci];
        }
        drmModeFreeEncoder(enc);
    }
    if (!crtc_id) {
        std::cerr << "[drm_virtual] no CRTC available\n";
        drmModeFreeConnector(conn);
        drmModeFreeResources(res);
        return false;
    }
    m.crtc_id = crtc_id;
    // Save current CRTC for restore on teardown.
    m.saved_crtc = drmModeGetCrtc(m.drm_fd, crtc_id);

    drmModeFreeConnector(conn);
    drmModeFreeResources(res);

    // Create dumb buffer (XRGB8888).
    drm_mode_create_dumb cd{};
    cd.width  = static_cast<uint32_t>(m.width);
    cd.height = static_cast<uint32_t>(m.height);
    cd.bpp    = 32;
    if (drmIoctl(m.drm_fd, DRM_IOCTL_MODE_CREATE_DUMB, &cd) != 0) {
        std::cerr << "[drm_virtual] CREATE_DUMB failed: "
                  << std::strerror(errno) << "\n";
        return false;
    }
    m.dumb_handle = cd.handle;
    m.dumb_pitch  = cd.pitch;
    m.dumb_size   = cd.size;

    // Add framebuffer.
    if (drmModeAddFB(m.drm_fd,
                     static_cast<uint32_t>(m.width),
                     static_cast<uint32_t>(m.height),
                     24, 32, m.dumb_pitch, m.dumb_handle, &m.fb_id) != 0) {
        std::cerr << "[drm_virtual] drmModeAddFB failed: "
                  << std::strerror(errno) << "\n";
        return false;
    }

    // Map framebuffer to userspace.
    drm_mode_map_dumb md{};
    md.handle = m.dumb_handle;
    if (drmIoctl(m.drm_fd, DRM_IOCTL_MODE_MAP_DUMB, &md) != 0) {
        std::cerr << "[drm_virtual] MAP_DUMB failed: "
                  << std::strerror(errno) << "\n";
        return false;
    }
    m.fb_map = ::mmap(nullptr, m.dumb_size,
                      PROT_READ, MAP_SHARED, m.drm_fd,
                      static_cast<off_t>(md.offset));
    if (m.fb_map == MAP_FAILED) {
        std::cerr << "[drm_virtual] mmap failed: "
                  << std::strerror(errno) << "\n";
        return false;
    }

    // Acquire DRM master to set the CRTC (drop it afterwards so compositors
    // can still take over; we only need it for the initial mode-set).
    const bool got_master = (drmSetMaster(m.drm_fd) == 0);
    if (!got_master) {
        std::cerr << "[drm_virtual] drmSetMaster failed: "
                  << std::strerror(errno)
                  << " — try running without a compositor holding master\n";
    }

    // Activate the virtual display: set CRTC.
    if (drmModeSetCrtc(m.drm_fd, m.crtc_id, m.fb_id, 0, 0,
                       &m.connector_id, 1, &m.mode_info) != 0) {
        if (got_master) drmDropMaster(m.drm_fd);
        std::cerr << "[drm_virtual] drmModeSetCrtc failed: "
                  << std::strerror(errno) << "\n";
        return false;
    }
    if (got_master) drmDropMaster(m.drm_fd);

    std::cerr << "[drm_virtual] virtual display active: "
              << m.width << "×" << m.height
              << " @ " << m.fps << " Hz\n";
    m.ready = true;
    return true;
}

void DrmVirtualCapture::teardown() {
    impl_->cleanup();
}

// ─── next_frame ──────────────────────────────────────────────────────────────

void DrmVirtualCapture::read_fb_to_nv12(pulsar::core::RawFrame& frame) {
    auto& m = *impl_;
    const auto* xrgb = static_cast<const uint32_t*>(m.fb_map);
    const size_t nv12_size =
        static_cast<size_t>(m.width * m.height) * 3 / 2;

    struct NV12Buf final : public pulsar::core::FrameBuffer {
        std::vector<uint8_t> d;
        explicit NV12Buf(size_t sz) : d(sz, 0) {}
        uint8_t* data() const override { return const_cast<uint8_t*>(d.data()); }
        size_t   size() const override { return d.size(); }
    };
    auto buf = std::make_shared<NV12Buf>(nv12_size);

    uint8_t* Y  = buf->d.data();
    uint8_t* UV = buf->d.data() + m.width * m.height;
    xrgb_to_nv12(xrgb, m.width, m.height, Y, UV);

    timespec ts{}; ::clock_gettime(CLOCK_MONOTONIC, &ts);
    frame.buffer  = std::move(buf);
    frame.width   = m.width;
    frame.height  = m.height;
    frame.pts_us  = static_cast<int64_t>(ts.tv_sec) * 1'000'000
                  + ts.tv_nsec / 1'000;
    frame.format  = pulsar::core::PixelFormat::NV12;
    frame.dirty_rects.push_back({0, 0, m.width, m.height});
}

std::optional<pulsar::core::RawFrame> DrmVirtualCapture::next_frame() {
    auto& m = *impl_;

    // Lazy initialisation: set up virtual display on first call.
    if (!m.ready) {
        if (!open_vkms_device() || !setup_virtual_display()) {
            return std::nullopt;
        }
    }

    // Frame-rate throttle.
    const auto interval = microseconds(1'000'000 / m.fps);
    const auto now      = steady_clock::now();
    if (now - m.last_frame_tp < interval)
        std::this_thread::sleep_for(interval - (now - m.last_frame_tp));
    m.last_frame_tp = steady_clock::now();

    // XOR checksum over the first 4 KB to detect unchanged frames.
    const auto* u64 = static_cast<const uint64_t*>(m.fb_map);
    const size_t n64 = std::min<size_t>(512, m.dumb_size / 8);
    uint64_t chk = 0;
    for (size_t i = 0; i < n64; ++i) chk ^= u64[i];
    if (chk == m.last_checksum) return std::nullopt; // unchanged
    m.last_checksum = chk;

    pulsar::core::RawFrame frame;
    read_fb_to_nv12(frame);
    return frame;
}

// ─── Other ICaptureSource stubs ──────────────────────────────────────────────

std::optional<pulsar::core::CursorState> DrmVirtualCapture::next_cursor() {
    return pulsar::core::CursorState{ .x = 0, .y = 0, .visible = false };
}

std::vector<pulsar::core::DisplayInfo> DrmVirtualCapture::enumerate_displays() const {
    if (!impl_->ready) return {};
    return {{ 0, "VKMS Virtual Display",
              impl_->width, impl_->height, impl_->fps, true, false }};
}

void DrmVirtualCapture::select_display(int) {}

int DrmVirtualCapture::display_refresh_rate() const {
    return impl_->fps;
}

void DrmVirtualCapture::set_event_callback(
        std::function<void(pulsar::core::CaptureEvent)> cb) {
    impl_->event_cb = std::move(cb);
}

pulsar::core::AdapterCapabilities DrmVirtualCapture::capabilities() const {
    pulsar::core::AdapterCapabilities c;
    c.output_formats    = { pulsar::core::PixelFormat::NV12 };
    c.color_spaces      = { pulsar::core::ColorSpace::BT709 };
    c.supports_headless = true;
    c.requires_display  = false;
    return c;
}

} // namespace pulsar::capture::drm_virtual
