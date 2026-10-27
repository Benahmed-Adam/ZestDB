#include <catch2/catch_all.hpp>
#include <iostream>
#include <sstream>

#include "Logger.hpp"

using namespace Zest;

TEST_CASE("levelToString returns correct strings", "[logger]") {
    REQUIRE(levelToString(LogLevel::Info) == "INFO");
    REQUIRE(levelToString(LogLevel::Debug) == "DEBUG");
    REQUIRE(levelToString(LogLevel::Warning) == "WARNING");
    REQUIRE(levelToString(LogLevel::Error) == "ERROR");
    REQUIRE(levelToString(LogLevel::Critical) == "CRITICAL");
}

TEST_CASE("levelToColorCode returns correct ANSI codes", "[logger]") {
    REQUIRE(levelToColorCode(LogLevel::Debug) == "\x1b[36m");
    REQUIRE(levelToColorCode(LogLevel::Warning) == "\x1b[33m");
    REQUIRE(levelToColorCode(LogLevel::Error) == "\x1b[31m");
    REQUIRE(levelToColorCode(LogLevel::Critical) == "\x1b[35m");
    REQUIRE(levelToColorCode(LogLevel::Info) == "\x1b[0m");
}

TEST_CASE("setLoggerDebugMode enable/disable", "[logger]") {
    setLoggerDebugMode(true);
    REQUIRE_NOTHROW(ZestLog(LogLevel::Debug, "test debug on"));
    setLoggerDebugMode(false);
    REQUIRE_NOTHROW(ZestLog(LogLevel::Debug, "test debug off"));
    setLoggerDebugMode(true);
}

TEST_CASE("ZestLog with empty message", "[logger]") {
    REQUIRE_NOTHROW(ZestLog(LogLevel::Info, ""));
}

TEST_CASE("ZestLog with special characters", "[logger]") {
    REQUIRE_NOTHROW(ZestLog(LogLevel::Warning, "test\nnewline\ttab"));
}

TEST_CASE("ZestLog escapes control characters", "[logger]") {
    std::ostringstream captured;
    auto* old = std::cout.rdbuf(captured.rdbuf());
    ZestLog(LogLevel::Warning, "key \x1b[2J\nforged line");
    std::cout.rdbuf(old);
    const std::string out = captured.str();
    REQUIRE(out.find("\\x1b[2J\\x0aforged line") != std::string::npos);
    REQUIRE(out.find("\x1b[2J") == std::string::npos);
}
