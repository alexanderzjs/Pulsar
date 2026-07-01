#pragma once

#include <string>
#include <vector>

namespace pulsar::core {

// Per-encoder benchmark result from a short encoding probe.
struct EncoderBenchmark {
    std::string backend;              // "nvenc" / "vaapi" / "x264" / "videotoolbox"
    float       encode_fps       = 0; // measured encoding throughput (frames/sec)
    float       latency_ms       = 0; // average encode latency (ms)
    float       latency_jitter_ms= 0; // latency variance — higher = less stable
    float       bitrate_efficiency=0; // actual_kbps / target_kbps (1.0 = perfect)
    bool        available        = false;
};

// Snapshot of server hardware at probe time.
struct HardwareProfile {
    int   cpu_cores       = 0;
    float cpu_freq_ghz    = 0;
    int   gpu_vram_mb     = 0;        // 0 = no dedicated GPU detected
    float gpu_utilization = 0;        // 0.0 – 1.0 (current load)
    int   available_memory_mb = 0;
};

// Optional network profile filled after a client connects.
struct NetworkProfile {
    float bandwidth_kbps  = 0;
    float rtt_ms          = 0;
    float loss_rate       = 0;
};

// Full server capability profile produced by IProfiler::run().
struct ServerProfile {
    HardwareProfile               hardware;
    std::vector<EncoderBenchmark> encoders; // all probed encoders, in priority order
    NetworkProfile                network;  // filled lazily after client connection
};

// Configuration for the profiler (mirrors the "profiler" section in config.json).
struct ProfilerConfig {
    bool enabled           = true;
    int  benchmark_frames  = 30;      // frames to encode per encoder during probe
    bool cache_result      = true;    // persist profile to disk between restarts
    int  cache_ttl_seconds = 3600;    // cache lifetime (re-probe after this)
    std::string cache_path = ".pulsar_profile_cache.json";
};

class IProfiler {
public:
    virtual ~IProfiler() = default;
    // Runs encoder benchmarks and collects hardware info.
    // Takes 1–3 seconds on first run; returns cached result if valid.
    virtual ServerProfile run() = 0;
};

// Compute a combined score for an encoder given network and hardware conditions.
// Higher is better; negative means the encoder should not be used.
float score_encoder(const EncoderBenchmark& bench,
                    const NetworkProfile&   net,
                    const HardwareProfile&  hw);

// Return the backend name with the highest score, or "x264" as absolute fallback.
std::string select_best_encoder(const ServerProfile& profile);

} // namespace pulsar::core
