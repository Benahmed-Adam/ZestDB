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

    ResultType getBy(const std::regex& reg);
    ResultType setBy(const std::regex& reg, const std::string& value);
    ResultType delBy(const std::regex& reg);

    void flush();
    void stop();

private:
    int getShardId(const std::string& key) const;

    Settings settings;
    int numShards;
    std::vector<std::unique_ptr<Shard>> shards;

    static constexpr auto hashFunction = std::hash<std::string> {};
};