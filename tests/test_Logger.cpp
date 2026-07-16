#include <catch2/catch_all.hpp>

#include "Logger.hpp"

using namespace Zest;

TEST_CASE("levelToString returns correct strings", "[logger]") {
    REQUIRE(levelToString(LogLevel::INFO) == "INFO");
    REQUIRE(levelToString(LogLevel::DEBUG) == "DEBUG");
    REQUIRE(levelToString(LogLevel::WARNING) == "WARNING");
    REQUIRE(levelToString(LogLevel::ERROR) == "ERROR");
    REQUIRE(levelToString(LogLevel::CRITICAL) == "CRITICAL");
}

TEST_CASE("levelToColorCode returns correct ANSI codes", "[logger]") {
    REQUIRE(levelToColorCode(LogLevel::DEBUG) == "\x1b[36m");
    REQUIRE(levelToColorCode(LogLevel::WARNING) == "\x1b[33m");
    REQUIRE(levelToColorCode(LogLevel::ERROR) == "\x1b[31m");
    REQUIRE(levelToColorCode(LogLevel::CRITICAL) == "\x1b[35m");
    REQUIRE(levelToColorCode(LogLevel::INFO) == "\x1b[0m");
}

TEST_CASE("setLoggerDebugMode enable/disable", "[logger]") {
    setLoggerDebugMode(true);
    REQUIRE_NOTHROW(ZestLog(LogLevel::DEBUG, "test debug on"));
    setLoggerDebugMode(false);
    REQUIRE_NOTHROW(ZestLog(LogLevel::DEBUG, "test debug off"));
    setLoggerDebugMode(true);
}

TEST_CASE("ZestLog with empty message", "[logger]") { REQUIRE_NOTHROW(ZestLog(LogLevel::INFO, "")); }

TEST_CASE("ZestLog with special characters", "[logger]") {
    REQUIRE_NOTHROW(ZestLog(LogLevel::WARNING, "test\nnewline\ttab"));
}
