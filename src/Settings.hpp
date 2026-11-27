#pragma once

#include <atomic>
#include <climits>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <regex>
#include <string>

namespace Zest {

    /// Which keys a pattern command (getby, setby, delby, count, keys) or a trigger applies to.
    struct ValidationRule {
        std::function<bool(std::string_view)> func; // the key matches when it returns true; empty means every key
        unsigned int limit = UINT_MAX;              // `lim <n>`, UINT_MAX without one
        // Set by ShardManager during a parallel command with a limit: the rows already taken by every shard
        std::atomic<unsigned int>* globalMatchCount = nullptr;
    };

    /// Permission level of a user; each role can run the commands of the ones before it.
    enum class Role {
        Read,
        Write,
        Admin,
        Replica // only for the replication link of a replica: no ordinary command
    };

    /// The result of every command, sent to the client as `{"affectedRows":..,"code":..,"response":..}`.
    struct ResultType {
        /// Part of the protocol, never renumbered: every code is described in docs/return-codes.md, keep it in sync.
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
            RateLimited = 304,
            UserExists = 305,
            UserNotFound = 306,
            InvalidUserName = 307,
            InvalidRole = 308,
            LastAdmin = 309,

            UnknownConfigParam = 400,
            InvalidConfigValue = 401,
            ConfigNotReloadable = 402,
            ReloadFailed = 403,

            InternalError = 500,
            ReadFailed = 501,
            WriteFailed = 502,
            WalWriteFailed = 503,
            ArchiveFailed = 504,
            RestoreFailed = 505,

            ScriptError = 600,
            ScriptNotFound = 601,
            FunctionNotFound = 602,
            TriggerRejected = 603,
            ScriptLimitExceeded = 604
        };
        Code code;
        std::string response = "";  // the data returned, or the error message of a script error; empty otherwise
        long long affectedRows = 0; // entries read or modified
    };

    /// Rate limits of a user; an absent limit means unlimited.
    struct UserLimits {
        std::optional<double> rateLimit;        // commands per second
        std::optional<double> burst;            // commands allowed at once; rateLimit when absent
        std::optional<uint64_t> bandwidthLimit; // request bytes per second

        bool unlimited() const {
            return !rateLimit && !bandwidthLimit;
        }
    };

    /// The `replication` block of config.yaml. Read at startup only: a reload cannot change it.
    struct ReplicationSettings {
        bool replica = false; // `role: replica`; every instance is a primary by default
        // Replica side: the primary's address and the user (with the `replica` role there) to log in with
        std::string primaryHost;
        uint16_t primaryPort = 0;
        std::string user;
        std::string password;
        // Primary side: the addresses allowed to replicate (a regex) and the number of events kept for a replica to catch up
        std::string allowedReplicasStr = ".*";
        size_t historySize = 100000;

        bool operator==(const ReplicationSettings&) const = default;
    };

    /// The configuration read from config.yaml (see docs/configuration.md for each field).
    /// Each shard keeps a copy in which DbPath, IndexPath and WalPath point to its own files.
    /// The initial values are the defaults of a field missing from config.yaml (loadConfig reads them from here).
    struct Settings {
        std::filesystem::path DbPath;
        std::filesystem::path IndexPath;
        std::filesystem::path WalPath;
        std::filesystem::path SSLCertPath;
        std::filesystem::path SSLKeyPath;
        std::filesystem::path ArchiveStoragePath;

        unsigned long SegSize = 128000;
        unsigned int MaxKeySize = 64;
        unsigned int MaxValueSize = 10000;
        unsigned int CacheSize = 1000;
        unsigned int CompactingInterval = 3600;
        unsigned int FlushInterval = 120;
        uint16_t DBPort = 7321;
        uint16_t WebPort = 1237;

        std::regex NetworkValidation;
        std::string NetworkValidationStr = ".*";

        unsigned int ArchiveCreationDelay = 3600;
        unsigned int ArchiveRetention = 10;

        unsigned int MaxConnections = 1024;
        unsigned int AuthTimeout = 10;
        unsigned int IdleTimeout = 300;

        bool AutoArchiveSaving = true;
        bool isDebug = false;
        bool useSSL = false;
        bool jsonOnly = false;
        bool readOnly = false;
        bool fsyncOnWrite = true;

        UserLimits defaultLimits; // the top-level rateLimit, burst and bandwidthLimit, for the users that set none

        ReplicationSettings replication;
        std::regex AllowedReplicas{ ".*" };
    };

} // namespace Zest
