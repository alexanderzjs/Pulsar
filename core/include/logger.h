#pragma once

#include <string>

namespace pulsar::core {

enum class LogLevel { Trace, Debug, Info, Warn, Error };

class ILogger {
public:
    virtual ~ILogger() = default;
    virtual void log(LogLevel level, const std::string& message) = 0;
};

} // namespace pulsar::core
