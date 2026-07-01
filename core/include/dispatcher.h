#pragma once

#include <string>

namespace pulsar::core {

enum class ProtocolHint { RDP, VNC, WebRTC, QUIC, RTP, Unknown };

class IConnectionDispatcher {
public:
    virtual ~IConnectionDispatcher() = default;
    virtual ProtocolHint detect_by_port(int port) const = 0;
};

} // namespace pulsar::core
