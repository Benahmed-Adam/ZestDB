#pragma once

#include <memory>
#include <string>
#include <vector>

#include "Settings.hpp"
#include "Shard.hpp"
#include "ThreadPool.hpp"

class ShardManager {
public:
    ShardManager(const Settings& baseSettings, int numShardsCount);
    ~ShardManager();

    ResultType get(const std::string& key);
    ResultType set(const std::string& key, const std::string& value);
    ResultType del(const std::string& key);

    ResultType getBy(const std::regex& reg, unsigned int limit);
    ResultType setBy(const std::regex& reg, const std::string& value, unsigned int limit);
    ResultType delBy(const std::regex& reg, unsigned int limit);

    void flush();
    void stop();

private:
    int getShardId(const std::string& key) const;

    Settings settings;
    int numShards;
    std::vector<std::unique_ptr<Shard>> shards;
    std::unique_ptr<ThreadPool> threadPool;

    static constexpr auto hashFunction = std::hash<std::string> {};
};