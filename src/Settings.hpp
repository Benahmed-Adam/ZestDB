#pragma once

#include <atomic>
#include <climits>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <regex>
#include <string>

namespace Zest {

    struct ValidationRule {
        std::function<bool(std::string_view)> func;
        unsigned int limit = UINT_MAX;
        std::atomic<unsigned int>* globalMatchCount = nullptr;
    };

    enum class Role {
        Read,
        Write,
        Admin
    };

    struct ResultType {
        enum class Code {
            Success = 0,

            NoCommand = 100,
            UnknownCommand = 101,
            MissingKey = 102,
            MissingValue = 103,
            MissingPattern = 104,
            MissingArguments = 105,
            InvalidMode = 106,
            InvalidRegex = 107,
            InvalidLimit = 108,
            InvalidTtl = 109,
            MalformedBatch = 110,
            PayloadTooLarge = 111,
            InvalidIncrement = 112,
            InvalidValueFormat = 113,

            KeyNotFound = 200,
            InvalidKey = 201,
            InvalidValue = 202,
            EmptyValue = 203,
            JsonOnly = 204,
            NotAnInteger = 205,
            IntegerOverflow = 206,
            KeyExists = 207,
            ValueMismatch = 208,
            ArchiveNotFound = 209,
            InvalidArchive = 210,

            AuthFailed = 300,
            PermissionDenied = 301,
            SessionRevoked = 302,
            ReadOnly = 303,

            UnknownConfigParam = 400,
            InvalidConfigValue = 401,
            ConfigNotReloadable = 402,
            ReloadFailed = 403,

            InternalError = 500,
            ReadFailed = 501,
            WriteFailed = 502,
            WalWriteFailed = 503,
            ArchiveFailed = 504,
            RestoreFailed = 505
        };
        Code code;
        std::string response = "";
        long long affectedRows = 0;
    };

    struct Settings {
        std::filesystem::path DbPath;
        std::filesystem::path IndexPath;
        std::filesystem::path WalPath;
        std::filesystem::path SSLCertPath;
        std::filesystem::path SSLKeyPath;
        std::filesystem::path ArchiveStoragePath;

        unsigned long SegSize;
        unsigned int MaxKeySize;
        unsigned int MaxValueSize;
        unsigned int CacheSize;
        unsigned int CompactingInterval;
        unsigned int FlushInterval;
        uint16_t DBPort;
        uint16_t WebPort;

        std::regex NetworkValidation;
        std::string NetworkValidationStr;

        unsigned int ArchiveCreationDelay;
        unsigned int ArchiveRetention;

        unsigned int MaxConnections;
        unsigned int AuthTimeout;
        unsigned int IdleTimeout;

        bool AutoArchiveSaving;
        bool isDebug;
        bool useSSL;
        bool jsonOnly;
        bool readOnly;
        bool fsyncOnWrite;
    };

} // namespace Zest
