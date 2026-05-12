#pragma once

#include <memory>
#include <string>
#include <vector>

#include "Settings.hpp"
#include "Shard.hpp"
#include "ThreadPool.hpp"

class ShardManager {
public:
    ShardManager(Settings& baseSettings, int numShardsCount);
    ~ShardManager();

    ResultType get(const std::string& key);
    ResultType set(const std::string& key, const std::string& value);
    ResultType del(const std::string& key);

    ResultType getBy(ValidationRule valid);
    ResultType setBy(ValidationRule valid, const std::string& value);
    ResultType delBy(ValidationRule valid);

    void flush();
    void stop();
    void clearAllWAL();
    void replayAllWAL(const std::function<std::string(const std::string&)>& execCmdFunc);
    void appendToWAL(const std::string& key, const std::string& command);
    
    void reloadSettings(Settings& set);
    
    const std::vector<std::unique_ptr<Shard>>& getShards() const { return this->shards; }

private:
    int getShardId(const std::string& key) const;

    Settings settings;
    int numShards;
    std::vector<std::unique_ptr<Shard>> shards;
    std::unique_ptr<ThreadPool> threadPool;

    static constexpr auto hashFunction = std::hash<std::string> {};
};