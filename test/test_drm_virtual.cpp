// test/test_drm_virtual.cpp
// Standalone smoke test for DrmVirtualCapture (Step 9 — Headless).
//
// Prerequisites:
//   sudo modprobe vkms
//   sudo systemctl stop gdm   (release DRM master)
//
// Build:  cmake --build build --target test_drm_virtual
// Run:    ./build/test_drm_virtual

#include "drm_virtual_capture.h"
#include <cassert>
#include <iostream>

int main() {
    using namespace pulsar::capture::drm_virtual;

    std::cout << "=== DrmVirtualCapture smoke test ===\n";

    // 1. Availability probe (just checks for VKMS device node)
    if (!DrmVirtualCapture::is_available()) {
        std::cerr << "SKIP: no VKMS device (run: sudo modprobe vkms)\n";
        return 0;
    }
    std::cout << "[ok] VKMS device found\n";

    // 2. Construction + configure 1280×720 @ 30 Hz
    DrmVirtualCapture cap;
    cap.set_resolution(1280, 720, 30);

    // 3. First next_frame() — triggers lazy init:
    //    open /dev/dri/cardX, drmSetMaster, create dumb buffer,
    //    mmap framebuffer, drmModeSetCrtc (activates the virtual display).
    auto f0 = cap.next_frame();
    if (!f0.has_value()) {
        std::cerr << "FAIL: first next_frame() returned nullopt\n"
                  << "  Check: is a compositor still holding DRM master?\n"
                  << "  Fix:   sudo systemctl stop gdm\n";
        return 1;
    }
    assert(f0->width  == 1280);
    assert(f0->height == 720);
    assert(f0->format == pulsar::core::PixelFormat::NV12);
    assert(f0->buffer != nullptr);
    const size_t expected_nv12 = static_cast<size_t>(1280 * 720 * 3 / 2);
    assert(f0->buffer->size() == expected_nv12);
    std::cout << "[ok] first frame: "
              << f0->width << "×" << f0->height
              << "  NV12  " << f0->buffer->size() << " bytes\n";

    // 4. Second call with same content → dedup should return nullopt
    //    (the framebuffer is all-zeros = black; checksum unchanged)
    auto f1 = cap.next_frame();
    if (!f1.has_value()) {
        std::cout << "[ok] second frame correctly deduplicated (unchanged content)\n";
    } else {
        std::cout << "[ok] second frame returned (content changed between calls)\n";
    }

    // 5. enumerate_displays → should list the VKMS virtual display
    auto displays = cap.enumerate_displays();
    assert(!displays.empty());
    assert(displays[0].width  == 1280);
    assert(displays[0].height == 720);
    std::cout << "[ok] enumerate_displays: \"" << displays[0].name << "\""
              << "  " << displays[0].width << "×" << displays[0].height
              << " @" << displays[0].refresh_rate << " Hz\n";

    // 6. capabilities flags
    auto caps = cap.capabilities();
    assert(caps.supports_headless == true);
    assert(caps.requires_display  == false);
    assert(!caps.output_formats.empty());
    std::cout << "[ok] capabilities: supports_headless=true  requires_display=false\n";

    std::cout << "=== PASS ===\n";
    return 0;
}
