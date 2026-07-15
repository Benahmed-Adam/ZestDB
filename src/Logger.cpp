#include <ctime>
#include <iostream>
#include <sstream>

#include "Logger.hpp"

namespace Zest {

static bool isDebug = true;

void setLoggerDebugMode(bool enabled)
{
    isDebug = enabled;
}

std::string levelToString(LogLevel level)
{
    switch (level) {
    case LogLevel::INFO:
        return "INFO";
    case LogLevel::DEBUG:
        return "DEBUG";
    case LogLevel::WARNING:
        return "WARNING";
    case LogLevel::ERROR:
        return "ERROR";
    case LogLevel::CRITICAL:
        return "CRITICAL";
    default:
        return "UNKNOWN";
    }
}

std::string levelToColorCode(LogLevel level)
{
    switch (level) {
    case LogLevel::DEBUG:
        return "\x1b[36m";
    case LogLevel::WARNING:
        return "\x1b[33m";
    case LogLevel::ERROR:
        return "\x1b[31m";
    case LogLevel::CRITICAL:
        return "\x1b[35m";
    default:
        return "\x1b[0m";
    }
}

void ZestLog(LogLevel level, const std::string& message)
{
    if (!isDebug && level == LogLevel::DEBUG)
        return;

    time_t now = time(0);
    tm* timeinfo = localtime(&now);
    char timestamp[20];
    strftime(timestamp, sizeof(timestamp),
        "%Y-%m-%d %H:%M:%S", timeinfo);

    std::ostringstream logEntry;
    logEntry << levelToColorCode(level)
             << "[" << timestamp << "] "
             << levelToString(level) << ": " << message
             << "\x1b[0m"
             << std::endl;

    std::cout << logEntry.str();
}

} // namespace Zest