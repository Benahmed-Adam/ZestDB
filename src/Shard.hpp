#pragma once

#include <memory>
#include <shared_mutex>

#include "Compactor.hpp"
#include "IndexManager.hpp"
#include "LRUCache.hpp"
#include "Settings.hpp"
#include "StorageManager.hpp"

class Shard {
public:
    Shard(const Settings& baseSettings, int shardIdNum);
    ~Shard();

    ResultType get(const std::string& key);
    ResultType set(const std::string& key, const std::string& value);
    ResultType del(const std::string& key);

    ResultType getBy(const std::regex& reg);
    ResultType setBy(const std::regex& reg, const std::string& value);
    ResultType delBy(const std::regex& reg);

    void flush();
    void stop();

    int getShardId() const { return shardId; }

    std::unique_ptr<IndexManager> indexManager;
    Settings settings;
    int shardId;

private:
    void boot();
    void fillCache();

    std::unique_ptr<StorageManager> storageManager;
    std::unique_ptr<LRUCache> cache;
    std::unique_ptr<Compactor> compactor;

    std::atomic<bool> initialized;
    std::atomic<bool> replaying;
    std::shared_mutex readMtx;
};