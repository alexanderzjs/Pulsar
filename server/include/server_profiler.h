#pragma once

#include "profiler.h"
#include "config.h"

namespace pulsar::server {

// Creates and runs the server profiler.
// If profiler.enabled is false, returns a minimal profile with all available
// encoders marked as available=true but with zero benchmark values.
pulsar::core::ServerProfile run_profiler(const ServerConfig& cfg);

} // namespace pulsar::server
