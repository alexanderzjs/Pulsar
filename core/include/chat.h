#pragma once
#include <cstdint>
#include <functional>
#include <string>

namespace pulsar::core {

struct ChatMessage {
    std::string client_id;
    std::string text;
    int64_t     timestamp_us = 0;
};

class IChatChannel {
public:
    virtual ~IChatChannel() = default;
    // Broadcast a message to all clients (exclude_sender = true by default).
    virtual void broadcast(const ChatMessage& msg, bool exclude_sender = true) = 0;
    virtual void set_message_callback(std::function<void(const ChatMessage&)> cb) = 0;
};

} // namespace pulsar::core
