#pragma once

#include <memory>
#include <shared_mutex>

#include "Compactor.hpp"
#include "IndexManager.hpp"
#include "LRUCache.hpp"
#include "PerfMonitoring.hpp"
#include "Settings.hpp"
#include "StorageManager.hpp"
#include "WAL.hpp"

class Shard {
public:
    Shard(Settings& baseSettings, int shardIdNum);
    ~Shard();

    ResultType get(const std::string& key);
    ResultType set(const std::string& key, const std::string& value);
    ResultType del(const std::string& key);

    ResultType getBy(ValidationRule& valid);
    ResultType setBy(ValidationRule& valid, const std::string& value);
    ResultType delBy(ValidationRule& valid);

    void flush();
    void stop();

    int getShardId() const { return this->shardId; }

    PerfMonitoring& getPerfMonitoring() { return this->perfMonitor; }
    WAL& getWAL() { return *this->wal; };
    StorageManager& getStorageManager() { return *this->storageManager; }

    void reloadSettings(Settings& set);

    std::unique_ptr<IndexManager> indexManager;
    Settings settings;
    int shardId;
    PerfMonitoring perfMonitor;

private:
    void boot();
    void fillCache();
    void verifyIndexEntries();

    std::unique_ptr<StorageManager> storageManager;
    std::unique_ptr<LRUCache> cache;
    std::unique_ptr<Compactor> compactor;
    std::unique_ptr<WAL> wal;

    std::atomic<bool> initialized;
    std::atomic<bool> replaying;
    std::atomic<bool> stopRequested;
    std::atomic<bool> stopped;
    std::shared_mutex readMtx;
};