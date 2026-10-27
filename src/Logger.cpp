#include "Logger.hpp"

#include <chrono>
#include <format>
#include <iostream>
#include <mutex>
#include <sstream>

namespace Zest {

    static bool isDebug = true;
    static std::mutex logMutex;

    void setLoggerDebugMode(bool enabled) {
        isDebug = enabled;
    }

    std::string levelToString(LogLevel level) {
        switch (level) {
        case LogLevel::Info:
            return "INFO";
        case LogLevel::Debug:
            return "DEBUG";
        case LogLevel::Warning:
            return "WARNING";
        case LogLevel::Error:
            return "ERROR";
        case LogLevel::Critical:
            return "CRITICAL";
        default:
            return "UNKNOWN";
        }
    }

    std::string levelToColorCode(LogLevel level) {
        switch (level) {
        case LogLevel::Debug:
            return "\x1b[36m";
        case LogLevel::Warning:
            return "\x1b[33m";
        case LogLevel::Error:
            return "\x1b[31m";
        case LogLevel::Critical:
            return "\x1b[35m";
        default:
            return "\x1b[0m";
        }
    }

    void ZestLog(LogLevel level, const std::string& message) {
        if (!isDebug && level == LogLevel::Debug)
            return;

        auto now = std::chrono::system_clock::now();

        std::string timestamp = std::format("{:%F %T}", now);

        std::string safeMessage;
        safeMessage.reserve(message.size());
        for (char ch : message) {
            const auto c = static_cast<unsigned char>(ch);
            if (c < 0x20 || c == 0x7f) {
                safeMessage += std::format("\\x{:02x}", c);
            } else {
                safeMessage += static_cast<char>(c);
            }
        }

        std::ostringstream logEntry;
        logEntry << levelToColorCode(level) << "[" << timestamp << "] " << levelToString(level) << ": " << safeMessage << "\x1b[0m" << std::endl;

        std::lock_guard<std::mutex> lock(logMutex);

        std::cout << logEntry.str();
    }

} // namespace Zest
