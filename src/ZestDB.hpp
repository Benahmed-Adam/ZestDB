#pragma once
#define CPPHTTPLIB_OPENSSL_SUPPORT

#include <atomic>
#include <memory>
#include <shared_mutex>
#include <unordered_map>

#include "Server.hpp"
#include "Settings.hpp"
#include "ShardManager.hpp"

#include "lib/httplib.hpp"

#define NUM_SHARDS 32

struct CreationValidationRuleResult {
    ValidationRule rule;
    bool result;
};

class ZestDB {
public:
    ZestDB();
    ~ZestDB();

    ResultType get(const std::string& key);
    ResultType set(const std::string& key, const std::string& value);
    ResultType del(const std::string& key);

    ResultType getBy(ValidationRule& valid);
    ResultType setBy(ValidationRule& valid, const std::string& value);
    ResultType delBy(ValidationRule& valid);

    ResultType execCmd(const std::string& command);
    bool validateToken(const std::string& username, const std::string& token) const;
    void stop();

    Settings settings;
    asio::io_context ioCtx;
    std::unique_ptr<httplib::Server> srv;

    static std::string responseToJson(const ResultType& resp);

private:
    void boot();
    bool validateKey(const std::string& key) const;
    bool validateValue(const std::string& value) const;
    bool isJsonValid(const std::string& value) const;
    void flush();
    void replayWAL();
    std::string help() const;
    CreationValidationRuleResult createValidationRule(const std::string& mode, const std::string& pattern) const;
    void appendToWAL(const std::string& key, const std::string& command);

    Settings loadConfig();
    ResultType reloadConfig();
    void setConfig();
    std::string getConfig() const;

    bool createArchive();

    std::unique_ptr<ShardManager> shardManager;
    std::unique_ptr<Server> socket;

    std::atomic<bool> initialized;
    std::atomic<bool> replaying;
    std::atomic<bool> isFlushing;

    std::unordered_map<std::string, std::string> users;
};

std::string sha256(const std::string& str);