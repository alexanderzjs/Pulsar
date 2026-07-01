#pragma once

#include "frame.h"
#include "audio.h"

namespace pulsar::core {

class IPacketSink {
public:
    virtual ~IPacketSink() = default;
    virtual std::string sink_id() const = 0;
    virtual void on_packet(EncodedPacket packet) = 0;
    virtual void on_audio(AudioPacket packet) = 0;
};

} // namespace pulsar::core
