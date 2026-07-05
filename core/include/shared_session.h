#pragma once
#include "input.h"
#include <string>
#include <vector>

namespace pulsar::core {

struct ClientDeviceBinding {
    std::string                   client_id;
    std::vector<InputEvent::Type> claimed_types; // device types this client controls
    int                           gamepad_id = -1;
};

// Routes client input events by device type.
// Each Transport calls allow() before injecting events into IInputHandler.
class IInputArbiter {
public:
    virtual ~IInputArbiter() = default;
    virtual void bind(const ClientDeviceBinding& binding) = 0;
    virtual void unbind(const std::string& client_id) = 0;
    // Returns true if the event type matches this client's claimed devices.
    virtual bool allow(const std::string& client_id, const InputEvent& event) const = 0;
    virtual std::vector<ClientDeviceBinding> list_bindings() const = 0;
};

} // namespace pulsar::core
