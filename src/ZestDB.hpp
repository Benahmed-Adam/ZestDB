#pragma once

#define CPPHTTPLIB_OPENSSL_SUPPORT

#include <atomic>
#include <condition_variable>
#include <memory>
#include <shared_mutex>
#include <thread>
#include <unordered_map>

#include "Server.hpp"
#include "Settings.hpp"
#include "ShardManager.hpp"
#include "lib/httplib.hpp"

#define NUM_SHARDS 32

namespace Zest {

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
        bool isAddressAllowed(const std::string& ip);
        void stop();

        Settings settings;
        asio::io_context ioCtx;
        std::unique_ptr<httplib::Server> srv;

        static std::string responseToJson(const ResultType& resp);

    private:
        void boot();
        bool validateKey(std::string_view key) const;
        bool validateValue(std::string_view value) const;
        bool isJsonValid(std::string_view value) const;
        void flush();
        void flushLocked();
        void replayWAL();
        std::string help() const;
        CreationValidationRuleResult createValidationRule(std::string_view mode, std::string_view pattern) const;
        bool appendToWAL(const std::string& key, const std::string& command);

        Settings loadConfig(std::unordered_map<std::string, std::string>& usersOut);
        ResultType reloadConfig();
        void setConfig();
        std::string getConfig() const;

        bool createArchive();

        std::unordered_map<std::string, std::string> users;
        mutable std::shared_mutex usersMtx;
        std::unique_ptr<ShardManager> shardManager;
        std::unique_ptr<Server> socket;

        std::atomic<bool> replaying;

        std::shared_mutex writeMtx;
        std::atomic<bool> isFlushing;

        std::mutex flushThreadMtx;
        std::mutex saveThreadMtx;
        std::condition_variable_any threadCV;

        std::jthread flushThread;
        std::jthread saveThread;
    };

    std::string sha256(const std::string& str);

} // namespace Zest
