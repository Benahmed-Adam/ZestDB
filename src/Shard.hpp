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

    /// One partition of the keyspace: a self-contained storage engine with its own index, value segments, cache, WAL,
    /// statistics and compactor thread, under `<DbPath>/shards/<id>/`.
    ///
    /// `readMtx` is shared by reads and taken exclusively by writes, so a read-modify-write (incr, setnx, cas) is atomic
    /// on its shard. The shard does not write its WAL: ZestDB logs each write once it succeeded.
    class Shard {
    public:
        /// Opens or creates the shard's files, checks the index against the segments, warms the cache and starts the compactor.
        Shard(Settings& baseSettings, int shardIdNum);
        /// Stops the compactor and flushes the data.
        ~Shard();

        /// Value of `key` from the cache, or from disk (then cached). KeyNotFound for a missing or expired key.
        ResultType get(const std::string& key);
        /// Stores `value`; `expiresAt` is an absolute date in ms, 0 for no expiry.
        ResultType set(const std::string& key, const std::string& value, uint64_t expiresAt = 0);
        /// Appends a tombstone for `key`. KeyNotFound if it does not exist.
        ResultType del(const std::string& key);

        /// Sets or (with 0) removes the expiry date of an existing key, without rewriting its value.
        ResultType setTTL(const std::string& key, uint64_t expiresAt);
        /// Seconds before `key` expires, rounded up, as the response: `-1` without expiry, `-2` if it does not exist.
        ResultType getTTL(const std::string& key);

        /// The matching keys and their values, as a JSON array of `{key: value}` objects.
        /// With a limit, `valid.globalMatchCount` counts the rows taken by every shard, so the limit holds across shards.
        ResultType getBy(ValidationRule& valid);
        /// Writes `value` to every matching key (same limit rules as getBy). `affectedRows` is the number of keys written.
        ResultType setBy(ValidationRule& valid, const std::string& value);
        /// Deletes every matching key (same limit rules as getBy). `affectedRows` is the number of keys deleted.
        ResultType delBy(ValidationRule& valid);

        /// Adds `delta` to the integer stored at `key` (a missing key counts as 0) and returns the new value as the response.
        /// `expiresAt` receives the key's expiry date, which is kept, so the caller can log it.
        ResultType incr(const std::string& key, int64_t delta, uint64_t& expiresAt);
        /// Stores `value` only if `key` does not exist (an expired key counts as absent); KeyExists otherwise.
        ResultType setnx(const std::string& key, const std::string& value);
        /// Writes `value` to an existing key and keeps its expiry date, returned in `expiresAt` so the caller can log it.
        /// KeyNotFound if the key does not exist (it is never created). Used by setby, which must not touch the TTL.
        ResultType replace(const std::string& key, const std::string& value, uint64_t& expiresAt);
        /// Replaces the value of `key` only if it equals `expected`; ValueMismatch otherwise. The expiry date is kept and returned in `expiresAt`.
        ResultType cas(const std::string& key, const std::string& expected, const std::string& value, uint64_t& expiresAt);

        /// `"true"` or `"false"` as the response, from the index only (no value is read).
        ResultType exists(const std::string& key);
        /// Number of live keys matching `valid` (every key when `valid.func` is empty), from the index only.
        size_t count(const ValidationRule& valid);

        /// Live keys matching `valid`; with `valid.limit`, the `limit` smallest ones, unsorted.
        /// `recordKeysStats` is false when the call serves another command (setby / delby with a limit), so it is not counted as a `keys`.
        std::vector<std::string> matchingKeys(const ValidationRule& valid, bool recordKeysStats = false);

        /// fsyncs the segments and the index. @return false if either failed.
        bool flushData();
        /// flushData(), then clears the WAL, which the synced files now make useless. The WAL is kept if the flush failed.
        void flush();
        /// Compacts the index, then rewrites the values of mostly empty segments and deletes the segments nothing points to.
        /// Takes `compactionMtx` and `readMtx` exclusively: the shard is blocked meanwhile.
        void compact();
        /// Stops the compactor thread and flushes the data. Safe to call twice.
        void stop();

        /// Held in shared mode by archiving, so no compaction rewrites the files while they are copied.
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

        /// Applies new settings (cache size, intervals...); the shard's own paths are kept.
        void reloadSettings(Settings& set);

        Settings settings; // a copy whose DbPath is the shard's folder, not the database's
        int shardId;
        PerfMonitoring perfMonitor;

    private:
        /// Creates the folder and files if needed, opens the components and starts the compactor thread.
        void boot();
        /// Warms the cache with up to `CacheSize` values on startup.
        void fillCache();
        /// Drops the index entries that point past the end of their segment, left by a crash between the two writes.
        void verifyIndexEntries();
        /// Second half of compact(): relocates the values out of segments less than half full of live data, then deletes unused segments.
        void compactSegments(std::vector<IndexEntry> entries);
        /// Writes a value; the caller holds `readMtx` exclusively.
        ResultType writeLocked(const std::string& key, const std::string& value, uint64_t expiresAt);
        /// Reads a value from the cache or the disk, and its expiry date; the caller holds `readMtx`.
        ResultType readLocked(const std::string& key, uint64_t& expiresAt);

        std::unique_ptr<WAL> wal;
        std::unique_ptr<IndexManager> indexManager;
        std::unique_ptr<StorageManager> storageManager;
        std::unique_ptr<LRUCache> cache;

        std::shared_mutex readMtx;       // shared by reads, exclusive for writes, flushes and compaction
        std::shared_mutex compactionMtx; // exclusive during compaction, shared while an archive copies the files

        std::atomic<bool> isStopped = false;
        std::mutex compactorThreadMtx;
        std::condition_variable_any threadCV;
        std::jthread compactorThread;
    };

} // namespace Zest
