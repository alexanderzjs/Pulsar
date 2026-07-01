#include "uinput_handler.h"

#include <fcntl.h>
#include <linux/uinput.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <cstring>

namespace pulsar::input::uinput {

UinputHandler::UinputHandler() {
    fd_ = ::open("/dev/uinput", O_WRONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd_ < 0) return;   // no permission — caller checks is_available()

    ::ioctl(fd_, UI_SET_EVBIT, EV_KEY);
    ::ioctl(fd_, UI_SET_EVBIT, EV_REL);
    ::ioctl(fd_, UI_SET_RELBIT, REL_X);
    ::ioctl(fd_, UI_SET_RELBIT, REL_Y);
    ::ioctl(fd_, UI_SET_RELBIT, REL_WHEEL);
    // Register a basic set of keys; a production driver would register all.
    for (int k = KEY_RESERVED; k < KEY_MAX; ++k)
        ::ioctl(fd_, UI_SET_KEYBIT, k);

    uinput_user_dev dev{};
    std::snprintf(dev.name, UINPUT_MAX_NAME_SIZE, "Pulsar Virtual Input");
    dev.id.bustype = BUS_USB;
    dev.id.vendor  = 0x045e;  // Microsoft
    dev.id.product = 0x0001;
    dev.id.version = 1;
    ::write(fd_, &dev, sizeof(dev));
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
        ::write(fd_, &ev, sizeof(ev));
        break;
    case pulsar::core::InputEvent::Type::MouseMove:
        ev.type = EV_REL; ev.code = REL_X; ev.value = event.value;
        ::write(fd_, &ev, sizeof(ev));
        ev.code = REL_Y;  ev.value = event.code;
        ::write(fd_, &ev, sizeof(ev));
        break;
    case pulsar::core::InputEvent::Type::MouseWheel:
        ev.type = EV_REL; ev.code = REL_WHEEL; ev.value = event.value;
        ::write(fd_, &ev, sizeof(ev));
        break;
    default:
        break;
    }
    // SYN_REPORT — commit the event batch.
    ev = {}; ev.type = EV_SYN; ev.code = SYN_REPORT;
    ::write(fd_, &ev, sizeof(ev));
}

bool UinputHandler::create_gamepad(int) { return false; }   // Phase 2
void UinputHandler::destroy_gamepad(int) {}

void UinputHandler::set_haptic_callback(
    std::function<void(pulsar::core::HapticCommand)> cb) {
    haptic_cb_ = std::move(cb);
}

} // namespace pulsar::input::uinput
