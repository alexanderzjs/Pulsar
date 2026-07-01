#pragma once

namespace pulsar::core {

struct PipelineMetrics {
    float   capture_fps            = 0.f;
    int64_t capture_latency_us     = 0;
    int     capture_dropped_frames = 0;

    float   encode_fps         = 0.f;
    int64_t encode_latency_us  = 0;
    float   encode_bitrate_kbps = 0.f;

    float   transport_rtt_ms          = 0.f;
    float   transport_loss_rate       = 0.f;
    float   transport_bandwidth_kbps  = 0.f;

    int64_t e2e_latency_us = 0;
};

class IMetricsCollector {
public:
    virtual ~IMetricsCollector() = default;
    virtual void report(const PipelineMetrics& m) = 0;
};

} // namespace pulsar::core
