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
    static constexpr const char* KEY_TOO_LONG = "The key is too long !";
    static constexpr const char* KEY_NOT_FOUND = "Key not found";
    static constexpr const char* VALUE_TOO_LONG = "The value is too long !";
    static constexpr const char* VALUE_EMPTY = "The value cannot be empty !";
    static constexpr const char* SUCCESS_SET = "Successfully set key: ";
    static constexpr const char* SUCCESS_DEL = "Successfully deleted key: ";
    static constexpr const char* PATTERN_EMPTY = "Pattern cannot be empty";
    static constexpr const char* INVALID_REGEX = "Invalid regex pattern";
    static constexpr const char* MISSING_KEY = "Error: missing key";
    static constexpr const char* MISSING_VALUE = "Error: missing value";
    static constexpr const char* MISSING_PATTERN = "Error: missing pattern";
    static constexpr const char* USAGE_GET = "Usage: get <key>";
    static constexpr const char* USAGE_SET = "Usage: set <key> <value>";
    static constexpr const char* USAGE_GETBY = "Usage: getby <mode> <pattern> [lim <n>]";
    static constexpr const char* USAGE_SETBY = "Usage: setby <mode> <pattern> <value> [lim <n>]";
    static constexpr const char* USAGE_DELBY = "Usage: delby <mode> <pattern> [lim <n>]";
    static constexpr const char* CMD_NOT_FOUND = "Command not found";
    static constexpr const char* TYPE_HELP = "Type h for help";
    static constexpr const char* FLUSH_SUCCESSFUL = "Flush successful !";
    static constexpr const char* JSON_ONLY_ERROR = "Invalid format, only json data allowed !";
    static constexpr const char* READ_ONLY_ERROR = "Modification not allowed ! The database is in read-only mode !";
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