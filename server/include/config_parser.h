#pragma once

#include "config.h"

#include <optional>
#include <string>

namespace pulsar::server {

// Loads config from a JSON file; populates error_msg on failure.
std::optional<ServerConfig> load_config(const std::string& path,
                                         std::string&       error_msg);

// Returns a default config (no file needed).
ServerConfig default_config();

} // namespace pulsar::server
