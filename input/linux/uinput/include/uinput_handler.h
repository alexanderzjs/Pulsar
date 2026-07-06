#pragma once

#include "input.h"

#include <unordered_map>

namespace pulsar::input::uinput {

// Linux uinput input injector.
// Injects keyboard, mouse and gamepad events via /dev/uinput.
class UinputHandler final : public pulsar::core::IInputHandler {
public:
    UinputHandler();
    ~UinputHandler() override;

    void inject(const pulsar::core::InputEvent& event) override;
    bool create_gamepad(int id) override;
    void destroy_gamepad(int id) override;
    void set_haptic_callback(std::function<void(pulsar::core::HapticCommand)> cb) override;

    bool is_available() const;
    size_t injected_events() const;

private:
    int    keyboard_fd_     = -1;
    int    mouse_fd_        = -1;
    int    absolute_mouse_fd_ = -1;
    size_t injected_events_ = 0;
    bool   has_abs_         = false;
    int    last_abs_x_      = 0;
    int    last_abs_y_      = 0;
    std::unordered_map<int, int> gamepad_fds_;
    std::function<void(pulsar::core::HapticCommand)> haptic_cb_;
};

} // namespace pulsar::input::uinput
