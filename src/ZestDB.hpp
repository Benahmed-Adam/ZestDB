#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <unordered_map>

#include "IndexManager.hpp"
#include "LRUCache.hpp"
#include "Settings.hpp"
#include "StorageManager.hpp"
#include "httplib.hpp"
#include "Compactor.hpp"

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

    Settings settings;
    httplib::Server srv;

private:
    void boot();
    void fillCache();
    bool validateKey(const std::string& key) const;
    bool validateValue(const std::string& value) const;
    bool handleRequest(const httplib::Request& req);

    std::unique_ptr<IndexManager> indexManager;
    std::unique_ptr<StorageManager> storageManager;
    std::unique_ptr<LRUCache> cache;
    std::unique_ptr<Compactor> compactor;

    std::mutex mtx;
    std::mutex cacheMtx;
    std::mutex indexMtx;
    std::atomic<bool> initialized;
    bool isCompacting;

    std::unordered_map<std::string, std::string> users;
};

std::string sha256(const std::string& str);