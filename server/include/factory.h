#pragma once

#include "config.h"

#include <string>

namespace pulsar::server {

// Start the server; blocks until the server stops.
int run_server(const ServerConfig& cfg);
int run_server(const std::string& config_path);

// Self-test: runs the pipeline for ~2 s on a loopback connection
// and verifies that video + audio packets flow end-to-end.
int run_verify(const ServerConfig& cfg);

} // namespace pulsar::server
