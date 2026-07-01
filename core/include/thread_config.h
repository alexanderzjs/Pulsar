#pragma once

namespace pulsar::core {

struct ThreadConfig {
    int priority    = 0;   // thread priority (higher = more important)
    int cpu_affinity = -1; // CPU core to pin to; -1 = no pinning
};

} // namespace pulsar::core
