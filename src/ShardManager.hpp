#pragma once

#include <memory>
#include <string>
#include <vector>

#include "Settings.hpp"
#include "Shard.hpp"

class ShardManager {
public:
    ShardManager(const Settings& baseSettings, int numShardsCount);
    ~ShardManager();

    ResultType get(const std::string& key);
    ResultType set(const std::string& key, const std::string& value);
    ResultType del(const std::string& key);

    ResultType getBy(const std::string& pattern);
    ResultType setBy(const std::string& pattern, const std::string& value);
    ResultType delBy(const std::string& pattern);

    void flush();
    void replayAllWAL();
    void stop();

private:
    int getShardId(const std::string& key) const;

    Settings settings;
    int numShards;
    std::vector<std::unique_ptr<Shard>> shards;

    static constexpr auto hashFunction = std::hash<std::string> {};
};