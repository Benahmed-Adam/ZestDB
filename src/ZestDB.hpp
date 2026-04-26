#pragma once
#define CPPHTTPLIB_OPENSSL_SUPPORT

#include <atomic>
#include <memory>
#include <shared_mutex>
#include <unordered_map>

#include "Server.hpp"
#include "Settings.hpp"
#include "ShardManager.hpp"
#include "httplib.hpp"
#include "WAL.hpp"

#define NUM_SHARDS 32

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
    bool validateToken(const std::string& username, const std::string& token) const;
    void stop();

    Settings settings;
    asio::io_context ioCtx;
    std::unique_ptr<httplib::Server> srv;

private:
    void boot();
    bool validateKey(const std::string& key) const;
    bool validateValue(const std::string& value) const;
    void flush();
    void replayWAL();

    std::string help() const;

    std::unique_ptr<ShardManager> shardManager;
    std::unique_ptr<Server> socket;
    std::unique_ptr<WAL> wal;

    std::atomic<bool> initialized;
    std::atomic<bool> replaying;

    std::unordered_map<std::string, std::string> users;
};

std::string sha256(const std::string& str);