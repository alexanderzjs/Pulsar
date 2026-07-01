#pragma once

#include "audio.h"
#include "frame.h"
#include "metrics.h"
#include "packet_sink.h"

#include <functional>
#include <string>
#include <vector>

namespace pulsar::core {

struct NetworkStats {
    float   rtt_ms            = 0.f;
    float   loss_rate         = 0.f;
    float   bandwidth_kbps    = 0.f;
    int64_t jitter_us         = 0;
    int     nack_requests     = 0;
    int     fec_recovered     = 0;
};

struct FecParams {
    bool enabled       = false;
    int  data_shards   = 10;
    int  parity_shards = 2;
};

enum class TransportEvent { Disconnected, Congested, Ready };

struct HapticCommand {
    int   gamepad_id  = 0;
    float low_freq    = 0.f;
    float high_freq   = 0.f;
    int   duration_ms = 0;
};

struct InputEvent;  // forward-declared; full definition in input.h

class ITransport : public IPacketSink {
public:
    virtual ~ITransport() = default;
    virtual bool connect(const std::string& endpoint) = 0;
    virtual void disconnect() = 0;
    virtual void send(EncodedPacket packet) = 0;
    virtual void send_batch(std::vector<EncodedPacket> packets) = 0;
    virtual void set_event_callback(std::function<void(TransportEvent)> cb) = 0;
    virtual void set_stats_callback(std::function<void(NetworkStats)> cb) = 0;
    virtual void set_jitter_buffer(int min_ms, int max_ms) = 0;
    virtual void set_fec_params(const FecParams& params) = 0;
    virtual void set_input_callback(std::function<void(InputEvent)> cb) = 0;
    virtual void send_audio(AudioPacket packet) = 0;
    virtual void send_haptic(const HapticCommand& cmd) = 0;
    virtual void send_stats(const PipelineMetrics& m) = 0;
    virtual void set_mic_callback(std::function<void(AudioFrame)> cb) = 0;
};

} // namespace pulsar::core
