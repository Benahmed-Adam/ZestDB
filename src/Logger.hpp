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

    /// ANSI escape sequence that colors a line of this level.
    std::string levelToColorCode(LogLevel level);
    /// Name of the level as printed in the log (`INFO`, `DEBUG`...).
    std::string levelToString(LogLevel level);

    /// Prints a timestamped, colored line to stdout. Thread-safe.
    /// Control characters of `message` are printed as `\xNN`, so a value or key cannot forge log lines.
    /// Debug lines are dropped unless the debug mode is on.
    void ZestLog(LogLevel level, const std::string& message);
    /// Turns the debug lines on or off (`isDebug`).
    void setLoggerDebugMode(bool enabled);

} // namespace Zest
