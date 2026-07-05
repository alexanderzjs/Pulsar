#pragma once
#include "packet_sink.h"
#include <string>

namespace pulsar::core {

// IRecorder inherits IPacketSink and can be registered with IOutputMultiplexer.
// Implementation writes video/audio to an MP4 container.
class IRecorder : public IPacketSink {
public:
    virtual void start(const std::string& output_path) = 0;
    virtual void stop() = 0;
    // on_packet / on_audio inherited from IPacketSink
};

} // namespace pulsar::core
