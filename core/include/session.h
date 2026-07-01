#pragma once

#include "encoder.h"

#include <string>
#include <unordered_map>

namespace pulsar::core {

enum class SessionState { Authenticating, Active, Suspended, Closed };

struct QosPolicy {
    int  max_bitrate_kbps          = 12000;
    int  max_fps                   = 60;
    int  priority                  = 0;
    int  max_cpu_percent           = -1;
    int  max_memory_mb             = -1;
    int  max_concurrent_sessions   = 1;
};

struct NegotiatedParams {
    int bitrate_kbps = 8000;
    int fps          = 60;
};

struct SessionConfig {
    std::string     encoder_backend;
    std::string     transport_backend;
    QosPolicy       qos{};
    NegotiatedParams negotiated{};
};

struct Session {
    std::string   id;
    SessionState  state  = SessionState::Authenticating;
    SessionConfig config{};
};

class ISessionManager {
public:
    virtual ~ISessionManager() = default;
    virtual Session         create(const AuthToken& token) = 0;
    virtual void            terminate(const std::string& session_id) = 0;
    virtual Session*        find(const std::string& session_id) = 0;
    virtual void            activate(const std::string& session_id) = 0;
    virtual QosPolicy       get_qos(const std::string& session_id) const = 0;
    virtual QosPolicy       default_qos() const = 0;
};

} // namespace pulsar::core
