#pragma once

#include <string>

namespace Zest {

    enum class LogLevel {
        Info,
        Debug,
        Warning,
        Error,
        Critical
    };

    std::string levelToColorCode(LogLevel level);
    std::string levelToString(LogLevel level);

    void ZestLog(LogLevel level, const std::string& message);
    void setLoggerDebugMode(bool enabled);

} // namespace Zest
