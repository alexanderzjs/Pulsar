#pragma once

#include "input.h"

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
    int    fd_              = -1;
    size_t injected_events_ = 0;
    std::function<void(pulsar::core::HapticCommand)> haptic_cb_;
};

} // namespace pulsar::input::uinput
