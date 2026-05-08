#pragma once

#include <climits>
#include <filesystem>
#include <regex>
#include <string>

struct ValidationRule {
    std::function<bool(const std::string&)> func;
    unsigned int limit = UINT_MAX;
};

struct ResultType {
    enum class Code {
        SUCCESS,
        ERROR
    };
    Code code;
    std::string message;
    long long affectedRows = 0;
};

struct Messages {
    static constexpr const unsigned int KEY_TOO_LONG     = 0;
    static constexpr const unsigned int KEY_NOT_FOUND    = 1;
    static constexpr const unsigned int VALUE_TOO_LONG   = 2;
    static constexpr const unsigned int VALUE_EMPTY      = 3;
    static constexpr const unsigned int SUCCESS          = 4;
    static constexpr const unsigned int FAIL             = 5;
    static constexpr const unsigned int PATTERN_EMPTY    = 7;
    static constexpr const unsigned int INVALID_REGEX    = 8;
    static constexpr const unsigned int MISSING_KEY      = 9;
    static constexpr const unsigned int MISSING_VALUE    = 10;
    static constexpr const unsigned int MISSING_PATTERN  = 11;
    static constexpr const unsigned int CMD_NOT_FOUND    = 12;
    static constexpr const unsigned int FLUSH_SUCCESSFUL = 14;
    static constexpr const unsigned int JSON_ONLY_ERROR  = 15;
    static constexpr const unsigned int READ_ONLY_ERROR  = 16;
    static constexpr const unsigned int NO_COMMAND_GIVEN = 17;
    static constexpr const unsigned int INVALID_MODE     = 18;
};

struct Settings {
    std::filesystem::path DbPath;
    std::filesystem::path IndexPath;
    std::filesystem::path WalPath;
    std::filesystem::path SSLCertPath;
    std::filesystem::path SSLKeyPath;

    unsigned long SegSize;
    unsigned int MaxKeySize;
    unsigned int MaxValueSize;
    unsigned int CacheSize;
    unsigned int CompactingInterval;
    unsigned int FlushInterval;
    short DBPort;
    short WebPort;

    std::regex NetworkValidation;
    std::string NetworkValidationStr;

    bool isDebug;
    bool useSSL;
    bool isRunning = true;
    bool jsonOnly;
    bool readOnly;

    bool hasCrashedLastTime;
};