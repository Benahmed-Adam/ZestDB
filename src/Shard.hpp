#pragma once

#include <condition_variable>
#include <memory>
#include <shared_mutex>
#include <thread>

#include "IndexManager.hpp"
#include "LRUCache.hpp"
#include "PerfMonitoring.hpp"
#include "Settings.hpp"
#include "StorageManager.hpp"
#include "WAL.hpp"

namespace Zest {

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
        std::vector<std::string> matchingKeys(const ValidationRule& valid);

        bool flushData();
        void flush();
        void stop();

        std::shared_mutex& compactionMutex() {
            return this->compactionMtx;
        }

        int getShardId() const {
            return this->shardId;
        }

        PerfMonitoring& getPerfMonitoring() {
            return this->perfMonitor;
        }

        WAL& getWAL() {
            return *this->wal;
        };

        StorageManager& getStorageManager() {
            return *this->storageManager;
        }

        void reloadSettings(Settings& set);

        Settings settings;
        int shardId;
        PerfMonitoring perfMonitor;

    private:
        void boot();
        void fillCache();
        void verifyIndexEntries();
        void compactSegments(std::vector<IndexEntry> entries);

        std::unique_ptr<WAL> wal;
        std::unique_ptr<IndexManager> indexManager;
        std::unique_ptr<StorageManager> storageManager;
        std::unique_ptr<LRUCache> cache;

        std::shared_mutex readMtx;
        std::shared_mutex compactionMtx;

        std::atomic<bool> isStopped = false;
        std::mutex compactorThreadMtx;
        std::condition_variable_any threadCV;
        std::jthread compactorThread;
    };

} // namespace Zest
