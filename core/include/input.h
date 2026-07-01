#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace pulsar::core {

struct TouchPoint {
    int   id       = 0;
    float x        = 0.f;
    float y        = 0.f;
    float pressure = 1.f;
    bool  down     = false;
};

struct HapticCommand;  // defined in transport.h

struct InputEvent {
    enum class Type {
        KeyDown, KeyUp,
        MouseMove, MouseButton, MouseWheel,
        GamepadButton, GamepadAxis,
        TouchBegin, TouchUpdate, TouchEnd,
        ClipboardText,
    };
    Type    type      = Type::KeyDown;
    int32_t code      = 0;
    int32_t value     = 0;
    int32_t gamepad_id = 0;
    std::vector<TouchPoint> touches;
};

class IInputHandler {
public:
    virtual ~IInputHandler() = default;
    virtual void inject(const InputEvent& event) = 0;
    virtual bool create_gamepad(int id) { return false; }
    virtual void destroy_gamepad(int id) { (void)id; }
    virtual void set_haptic_callback(std::function<void(struct HapticCommand)> cb) { (void)cb; }
    virtual void set_clipboard(const std::string& text) { (void)text; }
    virtual std::string get_clipboard() const { return {}; }
};

} // namespace pulsar::core
