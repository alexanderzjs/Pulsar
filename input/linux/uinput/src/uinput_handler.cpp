#include "uinput_handler.h"

#include <fcntl.h>
#include <linux/uinput.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <iostream>

namespace pulsar::input::uinput {

static bool write_event_checked(int fd, const input_event& ev) {
    for (;;) {
        ssize_t n = ::write(fd, &ev, sizeof(ev));
        if (n == static_cast<ssize_t>(sizeof(ev))) return true;
        if (n < 0 && errno == EINTR) continue;
        static int log_count = 0;
        if (log_count < 20) {
            std::cerr << "[uinput] write event failed: " << std::strerror(errno)
                      << " type=" << ev.type << " code=" << ev.code
                      << " value=" << ev.value << "\n";
            ++log_count;
        }
        return false;
    }
}

UinputHandler::UinputHandler() {
    fd_ = ::open("/dev/uinput", O_WRONLY | O_CLOEXEC);
    if (fd_ < 0) {
        std::cerr << "[uinput] open(/dev/uinput) failed: "
                  << std::strerror(errno)
                  << " (run with proper udev permissions or root)\n";
        return;
    }

    ::ioctl(fd_, UI_SET_EVBIT, EV_KEY);
    ::ioctl(fd_, UI_SET_EVBIT, EV_REL);
    ::ioctl(fd_, UI_SET_PROPBIT, INPUT_PROP_POINTER);
    ::ioctl(fd_, UI_SET_RELBIT, REL_X);
    ::ioctl(fd_, UI_SET_RELBIT, REL_Y);
    ::ioctl(fd_, UI_SET_RELBIT, REL_WHEEL);
    ::ioctl(fd_, UI_SET_KEYBIT, BTN_MOUSE);
    ::ioctl(fd_, UI_SET_KEYBIT, BTN_LEFT);
    ::ioctl(fd_, UI_SET_KEYBIT, BTN_RIGHT);
    ::ioctl(fd_, UI_SET_KEYBIT, BTN_MIDDLE);
    // Register a basic set of keys; a production driver would register all.
    for (int k = KEY_RESERVED; k < KEY_MAX; ++k)
        ::ioctl(fd_, UI_SET_KEYBIT, k);

    uinput_user_dev dev{};
    std::snprintf(dev.name, UINPUT_MAX_NAME_SIZE, "Pulsar Virtual Input");
    dev.id.bustype = BUS_USB;
    dev.id.vendor  = 0x045e;  // Microsoft
    dev.id.product = 0x0001;
    dev.id.version = 1;
    (void)::write(fd_, &dev, sizeof(dev));
    ::ioctl(fd_, UI_DEV_CREATE);
}

UinputHandler::~UinputHandler() {
    if (fd_ >= 0) {
        ::ioctl(fd_, UI_DEV_DESTROY);
        ::close(fd_);
    }
}

bool UinputHandler::is_available() const { return fd_ >= 0; }
size_t UinputHandler::injected_events() const { return injected_events_; }

void UinputHandler::inject(const pulsar::core::InputEvent& event) {
    ++injected_events_;
    if (fd_ < 0) return;

    input_event ev{};
    switch (event.type) {
    case pulsar::core::InputEvent::Type::KeyDown:
    case pulsar::core::InputEvent::Type::KeyUp:
        ev.type  = EV_KEY;
        ev.code  = static_cast<uint16_t>(event.code);
        ev.value = (event.type == pulsar::core::InputEvent::Type::KeyDown) ? 1 : 0;
        write_event_checked(fd_, ev);
        break;
    case pulsar::core::InputEvent::Type::MouseMove:
    {
        int dx = event.code;
        int dy = event.value;
        // Backward compatibility: legacy path packed absolute x/y into code.
        if ((event.code & 0xFFFF0000) != 0 && event.value == 0) {
            const int x = event.code & 0xFFFF;
            const int y = (event.code >> 16) & 0xFFFF;
            if (!has_abs_pointer_) {
                last_abs_x_ = x;
                last_abs_y_ = y;
                has_abs_pointer_ = true;
                break;
            }
            dx = x - last_abs_x_;
            dy = y - last_abs_y_;
            last_abs_x_ = x;
            last_abs_y_ = y;
        }
        if (dx == 0 && dy == 0) break;
        ev.type = EV_REL; ev.code = REL_X; ev.value = dx;
        write_event_checked(fd_, ev);
        ev.code = REL_Y;  ev.value = dy;
        write_event_checked(fd_, ev);
        break;
    }
    case pulsar::core::InputEvent::Type::MouseButton:
        ev.type = EV_KEY;
        if (event.code == 1) ev.code = BTN_LEFT;
        else if (event.code == 2) ev.code = BTN_MIDDLE;
        else if (event.code == 3) ev.code = BTN_RIGHT;
        else break;
        ev.value = (event.value != 0) ? 1 : 0;
        write_event_checked(fd_, ev);
        break;
    case pulsar::core::InputEvent::Type::MouseWheel:
        ev.type = EV_REL; ev.code = REL_WHEEL; ev.value = event.value;
        write_event_checked(fd_, ev);
        break;
    default:
        break;
    }
    // SYN_REPORT — commit the event batch.
    ev = {}; ev.type = EV_SYN; ev.code = SYN_REPORT;
    write_event_checked(fd_, ev);
}

bool UinputHandler::create_gamepad(int) { return false; }   // Phase 2
void UinputHandler::destroy_gamepad(int) {}

void UinputHandler::set_haptic_callback(
    std::function<void(pulsar::core::HapticCommand)> cb) {
    haptic_cb_ = std::move(cb);
}

} // namespace pulsar::input::uinput
