#pragma once
#include "packet_sink.h"
#include <memory>
#include <string>

namespace pulsar::core {

class IOutputMultiplexer {
public:
    virtual ~IOutputMultiplexer() = default;
    // Register a sink (ITransport or IRecorder).
    virtual void add_sink(std::shared_ptr<IPacketSink> sink) = 0;
    virtual void remove_sink(const std::string& sink_id) = 0;
    // Broadcast encoded packet to all sinks via shared_ptr (zero-copy).
    virtual void broadcast(EncodedPacket packet) = 0;
    virtual void broadcast_audio(AudioPacket packet) = 0;
};

} // namespace pulsar::core
