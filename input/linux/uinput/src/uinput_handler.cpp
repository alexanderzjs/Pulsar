#include "uinput_handler.h"

#include <cstdlib>
#include <fcntl.h>
#include <linux/uinput.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <iostream>
#include <string>
#include <string_view>

namespace {

std::string seat_scoped_name(std::string_view base_name) {
    const char* seat = std::getenv("XDG_SEAT");
    if (!seat || seat[0] == '\0' || std::strcmp(seat, "seat0") == 0) {
        return std::string(base_name);
    }

    std::string name;
    name.reserve(base_name.size() + std::strlen(seat) + 3);
    name.append(base_name);
    name.append(" (");
    name.append(seat);
    name.push_back(')');
    return name;
}

} // namespace

namespace pulsar::input::uinput {

static int create_device(const char* name, const std::function<void(int)>& configure) {
    int fd = ::open("/dev/uinput", O_WRONLY | O_CLOEXEC);
    if (fd < 0) {
        std::cerr << "[uinput] open(/dev/uinput) failed: "
                  << std::strerror(errno)
                  << " (run with proper udev permissions or root)\n";
        return -1;
    }

    configure(fd);

    uinput_user_dev dev{};
    std::snprintf(dev.name, UINPUT_MAX_NAME_SIZE, "%s", name);
    dev.id.bustype = BUS_USB;
    dev.id.vendor  = 0x045e;
    dev.id.product = 0x0001;
    dev.id.version = 1;

    const ssize_t wrote = ::write(fd, &dev, sizeof(dev));
    if (wrote != static_cast<ssize_t>(sizeof(dev))) {
        std::cerr << "[uinput] write uinput_user_dev failed: "
                  << std::strerror(errno) << "\n";
        ::close(fd);
        return -1;
    }

    if (::ioctl(fd, UI_DEV_CREATE) < 0) {
        std::cerr << "[uinput] UI_DEV_CREATE failed: "
                  << std::strerror(errno) << "\n";
        ::close(fd);
        return -1;
    }
    return fd;
}

static int create_absolute_mouse_device(const char* name, const std::function<void(int)>& configure) {
    int fd = ::open("/dev/uinput", O_WRONLY | O_CLOEXEC);
    if (fd < 0) {
        std::cerr << "[uinput] open(/dev/uinput) failed: "
                  << std::strerror(errno)
                  << " (run with proper udev permissions or root)\n";
        return -1;
    }

    configure(fd);

    uinput_user_dev dev{};
    std::snprintf(dev.name, UINPUT_MAX_NAME_SIZE, "%s", name);
    dev.id.bustype = BUS_USB;
    dev.id.vendor  = 0x045e;
    dev.id.product = 0x0002;
    dev.id.version = 1;
    dev.absmin[ABS_X] = 0;
    dev.absmax[ABS_X] = 65535;
    dev.absmin[ABS_Y] = 0;
    dev.absmax[ABS_Y] = 65535;

    const ssize_t wrote = ::write(fd, &dev, sizeof(dev));
    if (wrote != static_cast<ssize_t>(sizeof(dev))) {
        std::cerr << "[uinput] write uinput_user_dev failed: "
                  << std::strerror(errno) << "\n";
        ::close(fd);
        return -1;
    }

    if (::ioctl(fd, UI_DEV_CREATE) < 0) {
        std::cerr << "[uinput] UI_DEV_CREATE failed: "
                  << std::strerror(errno) << "\n";
        ::close(fd);
        return -1;
    }
    return fd;
}

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
    const auto mouse_name = seat_scoped_name("Pulsar Virtual Mouse");
    const auto absolute_mouse_name = seat_scoped_name("Pulsar Virtual Absolute Mouse");
    const auto keyboard_name = seat_scoped_name("Pulsar Virtual Keyboard");

    mouse_fd_ = create_device(mouse_name.c_str(), [](int fd) {
        ::ioctl(fd, UI_SET_EVBIT, EV_KEY);
        ::ioctl(fd, UI_SET_EVBIT, EV_REL);
        ::ioctl(fd, UI_SET_PROPBIT, INPUT_PROP_POINTER);
        ::ioctl(fd, UI_SET_RELBIT, REL_X);
        ::ioctl(fd, UI_SET_RELBIT, REL_Y);
        ::ioctl(fd, UI_SET_RELBIT, REL_WHEEL);
        ::ioctl(fd, UI_SET_KEYBIT, BTN_MOUSE);
        ::ioctl(fd, UI_SET_KEYBIT, BTN_LEFT);
        ::ioctl(fd, UI_SET_KEYBIT, BTN_RIGHT);
        ::ioctl(fd, UI_SET_KEYBIT, BTN_MIDDLE);
    });

    absolute_mouse_fd_ = create_absolute_mouse_device(absolute_mouse_name.c_str(), [](int fd) {
        ::ioctl(fd, UI_SET_EVBIT, EV_KEY);
        ::ioctl(fd, UI_SET_EVBIT, EV_ABS);
        ::ioctl(fd, UI_SET_PROPBIT, INPUT_PROP_DIRECT);
        ::ioctl(fd, UI_SET_ABSBIT, ABS_X);
        ::ioctl(fd, UI_SET_ABSBIT, ABS_Y);
        ::ioctl(fd, UI_SET_KEYBIT, BTN_MOUSE);
        ::ioctl(fd, UI_SET_KEYBIT, BTN_LEFT);
        ::ioctl(fd, UI_SET_KEYBIT, BTN_RIGHT);
        ::ioctl(fd, UI_SET_KEYBIT, BTN_MIDDLE);
    });

    keyboard_fd_ = create_device(keyboard_name.c_str(), [](int fd) {
        ::ioctl(fd, UI_SET_EVBIT, EV_KEY);
        ::ioctl(fd, UI_SET_PROPBIT, INPUT_PROP_DIRECT);
        for (int k = KEY_RESERVED; k < KEY_MAX; ++k)
            ::ioctl(fd, UI_SET_KEYBIT, k);
    });
}

UinputHandler::~UinputHandler() {
    for (auto& [id, fd] : gamepad_fds_) {
        if (fd >= 0) {
            ::ioctl(fd, UI_DEV_DESTROY);
            ::close(fd);
        }
        (void)id;
    }
    gamepad_fds_.clear();
    if (mouse_fd_ >= 0) {
        ::ioctl(mouse_fd_, UI_DEV_DESTROY);
        ::close(mouse_fd_);
    }
    if (keyboard_fd_ >= 0) {
        ::ioctl(keyboard_fd_, UI_DEV_DESTROY);
        ::close(keyboard_fd_);
    }
}

bool UinputHandler::is_available() const { return mouse_fd_ >= 0 && keyboard_fd_ >= 0; }
size_t UinputHandler::injected_events() const { return injected_events_; }

void UinputHandler::inject(const pulsar::core::InputEvent& event) {
    ++injected_events_;
    const bool is_keyboard =
        event.type == pulsar::core::InputEvent::Type::KeyDown ||
        event.type == pulsar::core::InputEvent::Type::KeyUp;
    int fd = is_keyboard ? keyboard_fd_ : mouse_fd_;
    if (fd < 0) return;

    static int log_count = 0;
    if (log_count < 20) {
        std::cerr << "[uinput] inject type=" << static_cast<int>(event.type)
                  << " code=" << event.code
                  << " value=" << event.value << "\n";
        ++log_count;
    }

    input_event ev{};
    switch (event.type) {
    case pulsar::core::InputEvent::Type::KeyDown:
    case pulsar::core::InputEvent::Type::KeyUp:
        ev.type  = EV_KEY;
        ev.code  = static_cast<uint16_t>(event.code);
        ev.value = (event.type == pulsar::core::InputEvent::Type::KeyDown) ? 1 : 0;
        write_event_checked(fd, ev);
        break;
    case pulsar::core::InputEvent::Type::MouseMove:
    {
        int dx = event.code;
        int dy = event.value;
        if (dx == 0 && dy == 0) break;
        ev.type = EV_REL; ev.code = REL_X; ev.value = dx;
        write_event_checked(fd, ev);
        ev.code = REL_Y;  ev.value = dy;
        write_event_checked(fd, ev);
        break;
    }
    case pulsar::core::InputEvent::Type::MouseAbsolute:
    {
        const int abs_x = event.code;
        const int abs_y = event.value;
        last_abs_x_ = abs_x;
        last_abs_y_ = abs_y;

        if (absolute_mouse_fd_ >= 0) {
            ev.type = EV_ABS; ev.code = ABS_X; ev.value = abs_x;
            write_event_checked(absolute_mouse_fd_, ev);
            ev.code = ABS_Y; ev.value = abs_y;
            write_event_checked(absolute_mouse_fd_, ev);
            fd = absolute_mouse_fd_;
        } else {
            ev.type = EV_REL; ev.code = REL_X; ev.value = abs_x - last_abs_x_;
            write_event_checked(mouse_fd_, ev);
            ev.code = REL_Y; ev.value = abs_y - last_abs_y_;
            write_event_checked(mouse_fd_, ev);
            fd = mouse_fd_;
        }
        last_abs_x_ = abs_x;
        last_abs_y_ = abs_y;
        has_abs_ = true;
        break;
    }
    case pulsar::core::InputEvent::Type::MouseButton:
        ev.type = EV_KEY;
        if (event.code == 1) ev.code = BTN_LEFT;
        else if (event.code == 2) ev.code = BTN_MIDDLE;
        else if (event.code == 3) ev.code = BTN_RIGHT;
        else break;
        ev.value = (event.value != 0) ? 1 : 0;
        if (absolute_mouse_fd_ >= 0) {
            write_event_checked(absolute_mouse_fd_, ev);
            fd = absolute_mouse_fd_;
        } else {
            write_event_checked(fd, ev);
        }
        break;
    case pulsar::core::InputEvent::Type::MouseWheel:
        ev.type = EV_REL; ev.code = REL_WHEEL; ev.value = event.value;
        write_event_checked(fd, ev);
        break;
    case pulsar::core::InputEvent::Type::GamepadButton:
    case pulsar::core::InputEvent::Type::GamepadAxis:
    {
        auto it = gamepad_fds_.find(event.gamepad_id);
        if (it == gamepad_fds_.end() || it->second < 0) break;
        if (event.type == pulsar::core::InputEvent::Type::GamepadButton) {
            ev.type = EV_KEY;
            ev.code = static_cast<uint16_t>(event.code);
            ev.value = event.value ? 1 : 0;
        } else {
            ev.type = EV_ABS;
            ev.code = static_cast<uint16_t>(event.code);
            ev.value = event.value;
        }
        write_event_checked(it->second, ev);
        break;
    }
    default:
        break;
    }
    // SYN_REPORT — commit the event batch.
    ev = {}; ev.type = EV_SYN; ev.code = SYN_REPORT;
    write_event_checked(fd, ev);
}

bool UinputHandler::create_gamepad(int id) {
    if (gamepad_fds_.count(id)) return true;
    int fd = create_device("Pulsar Virtual Gamepad", [](int fd) {
        ::ioctl(fd, UI_SET_EVBIT, EV_KEY);
        ::ioctl(fd, UI_SET_EVBIT, EV_ABS);
        ::ioctl(fd, UI_SET_KEYBIT, BTN_SOUTH);
        ::ioctl(fd, UI_SET_KEYBIT, BTN_EAST);
        ::ioctl(fd, UI_SET_KEYBIT, BTN_NORTH);
        ::ioctl(fd, UI_SET_KEYBIT, BTN_WEST);
        ::ioctl(fd, UI_SET_KEYBIT, BTN_TL);
        ::ioctl(fd, UI_SET_KEYBIT, BTN_TR);
        ::ioctl(fd, UI_SET_KEYBIT, BTN_SELECT);
        ::ioctl(fd, UI_SET_KEYBIT, BTN_START);
        ::ioctl(fd, UI_SET_KEYBIT, BTN_THUMBL);
        ::ioctl(fd, UI_SET_KEYBIT, BTN_THUMBR);
        ::ioctl(fd, UI_SET_ABSBIT, ABS_X);
        ::ioctl(fd, UI_SET_ABSBIT, ABS_Y);
        ::ioctl(fd, UI_SET_ABSBIT, ABS_RX);
        ::ioctl(fd, UI_SET_ABSBIT, ABS_RY);
        ::ioctl(fd, UI_SET_ABSBIT, ABS_Z);
        ::ioctl(fd, UI_SET_ABSBIT, ABS_RZ);
    });
    if (fd < 0) return false;
    gamepad_fds_[id] = fd;
    return true;
}
void UinputHandler::destroy_gamepad(int id) {
    auto it = gamepad_fds_.find(id);
    if (it == gamepad_fds_.end()) return;
    if (it->second >= 0) {
        ::ioctl(it->second, UI_DEV_DESTROY);
        ::close(it->second);
    }
    gamepad_fds_.erase(it);
}

void UinputHandler::set_haptic_callback(
    std::function<void(pulsar::core::HapticCommand)> cb) {
    haptic_cb_ = std::move(cb);
}

} // namespace pulsar::input::uinput
