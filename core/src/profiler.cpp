#include "profiler.h"
#include <algorithm>
#include <cmath>

namespace pulsar::core {

float score_encoder(const EncoderBenchmark& bench,
                    const NetworkProfile&   net,
                    const HardwareProfile&  hw)
{
    if (!bench.available) return -1.0f;

    float score = 0.0f;

    // Stability (lower jitter = better): weight 40 %
    score += (1.0f - std::min(bench.latency_jitter_ms / 10.0f, 1.0f)) * 40.0f;

    // Bitrate efficiency (actual/target close to 1.0 = better): weight 30 %
    score += (1.0f - std::min(std::abs(1.0f - bench.bitrate_efficiency), 1.0f)) * 30.0f;

    // Throughput (normalised to 60 fps): weight 20 %
    score += std::min(bench.encode_fps / 60.0f, 1.0f) * 20.0f;

    // Availability bonus: hardware encoders get +10 base for lower CPU cost
    if (bench.backend != "x264") score += 10.0f;

    // Network penalty: poor bandwidth → penalise encoders with low efficiency
    if (net.bandwidth_kbps > 0 && net.bandwidth_kbps < 8000)
        score -= (1.0f - bench.bitrate_efficiency) * 20.0f;

    // GPU overload penalty: when GPU > 80 % busy, hw encoder jitter spikes
    if (hw.gpu_utilization > 0.8f && bench.backend != "x264")
        score -= 30.0f;

    return score;
}

std::string select_best_encoder(const ServerProfile& profile) {
    std::string best = "x264";
    float       best_score = -1.0f;
    for (const auto& bench : profile.encoders) {
        float s = score_encoder(bench, profile.network, profile.hardware);
        if (s > best_score) {
            best_score = s;
            best       = bench.backend;
        }
    }
    return best;
}

} // namespace pulsar::core
