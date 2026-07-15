#pragma once

#include <string>

namespace Zest {

enum class LogLevel {
    INFO,
    DEBUG,
    WARNING,
    ERROR,
    CRITICAL
};

std::string levelToColorCode(LogLevel level);
std::string levelToString(LogLevel level);

void ZestLog(LogLevel level, const std::string& message);
void setLoggerDebugMode(bool enabled);

} // namespace Zest
