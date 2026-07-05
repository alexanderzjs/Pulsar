#pragma once
#include <string>

namespace pulsar::core {

class IWakeOnLan {
public:
    virtual ~IWakeOnLan() = default;
    // Listen for incoming Magic Packets on UDP port (default 9).
    virtual void listen(int port = 9) = 0;
    virtual void stop() = 0;
    // Send a Magic Packet to wake the target MAC address.
    virtual void send(const std::string& mac_address,
                      const std::string& broadcast_addr = "255.255.255.255") = 0;
};

} // namespace pulsar::core
