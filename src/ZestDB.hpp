#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <unordered_map>

#include "Compactor.hpp"
#include "IndexManager.hpp"
#include "LRUCache.hpp"
#include "Server.hpp"
#include "Settings.hpp"
#include "StorageManager.hpp"
#include "httplib.hpp"

struct ResultType {
    enum class Code {
        SUCCESS,
        ERROR
    };
    Code code;
    std::string message;
};

struct Messages {
    static constexpr const char* KEY_TOO_LONG = "The key is too long !";
    static constexpr const char* KEY_VALIDATION_FAILED = "The key does not respect the KeyValidation regex !";
    static constexpr const char* KEY_NOT_FOUND = "Key not found";
    static constexpr const char* VALUE_TOO_LONG = "The value is too long !";
    static constexpr const char* VALUE_VALIDATION_FAILED = "The value does not respect the ValueValidation regex !";
    static constexpr const char* SUCCESS_SET = "Successfully set key: ";
    static constexpr const char* SUCCESS_DEL = "Successfully deleted key: ";
    static constexpr const char* PATTERN_EMPTY = "Pattern cannot be empty";
    static constexpr const char* INVALID_REGEX = "Invalid regex pattern";
    static constexpr const char* MISSING_KEY = "Error: missing key";
    static constexpr const char* MISSING_VALUE = "Error: missing value";
    static constexpr const char* MISSING_PATTERN = "Error: missing pattern";
    static constexpr const char* USAGE_GET = "Usage: get <key>";
    static constexpr const char* USAGE_SET = "Usage: set <key> <value>";
    static constexpr const char* USAGE_GETBY = "Usage: getby <pattern>";
    static constexpr const char* USAGE_SETBY = "Usage: setby <pattern> <value>";
    static constexpr const char* USAGE_DELBY = "Usage: delby <pattern>";
    static constexpr const char* CMD_NOT_FOUND = "Command not found";
    static constexpr const char* TYPE_HELP = "Type h for help";
};

class ZestDB {
public:
    ZestDB();
    ~ZestDB();

    ResultType get(const std::string& key);
    ResultType set(const std::string& key, const std::string& value);
    ResultType del(const std::string& key);

    ResultType getBy(const std::string& patern);
    ResultType setBy(const std::string& patern, const std::string& value);
    ResultType delBy(const std::string& patern);

    std::string execCmd(const std::string& command);
    void stop();

    Settings settings;
    asio::io_context ioCtx;
    httplib::Server srv;
private:
    void boot();
    void fillCache();
    bool validateKey(const std::string& key) const;
    bool validateValue(const std::string& value) const;

    std::string help() const;

    std::unique_ptr<IndexManager> indexManager;
    std::unique_ptr<StorageManager> storageManager;
    std::unique_ptr<LRUCache> cache;
    std::unique_ptr<Compactor> compactor;
    std::unique_ptr<Server> socket;

    std::atomic<bool> initialized;
    std::mutex readMtx;

    std::unordered_map<std::string, std::string> users;
};

std::string sha256(const std::string& str);