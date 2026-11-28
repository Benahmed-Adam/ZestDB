#pragma once

#include <memory>
#include <shared_mutex>
#include <string>
#include <vector>

#include "Settings.hpp"
#include "Shard.hpp"
#include "ThreadPool.hpp"

namespace Zest {

    /// Owns the shards, sends each key to its shard, and runs the pattern commands on every shard in parallel.
    /// Single-key methods return their shard's result unchanged; see Shard for what each one does.
    class ShardManager {
    public:
        /// Opens `numShardsCount` shards under `<DbPath>/shards/` and a thread pool with one thread per hardware thread.
        ShardManager(Settings& baseSettings, int numShardsCount);
        /// Flushes and stops every shard.
        ~ShardManager();

        ResultType get(const std::string& key);
        ResultType set(const std::string& key, const std::string& value, uint64_t expiresAt = 0);
        ResultType del(const std::string& key);

        ResultType setTTL(const std::string& key, uint64_t expiresAt);
        ResultType getTTL(const std::string& key);

        /// Runs getBy on every shard in parallel and merges their JSON arrays; the limit, if any, is shared by all shards.
        ResultType getBy(ValidationRule& valid);
        /// Without a limit, runs setBy on every shard in parallel. With a limit, writes the `limit` smallest matching keys,
        /// so replaying the same command from the WAL picks the same keys.
        ResultType setBy(ValidationRule& valid, const std::string& value);
        /// Like setBy: parallel without a limit, the `limit` smallest matching keys with one.
        ResultType delBy(ValidationRule& valid);

        ResultType incr(const std::string& key, int64_t delta, uint64_t& expiresAt);
        ResultType setnx(const std::string& key, const std::string& value);
        ResultType replace(const std::string& key, const std::string& value, uint64_t& expiresAt);
        ResultType cas(const std::string& key, const std::string& expected, const std::string& value, uint64_t& expiresAt);

        ResultType exists(const std::string& key);
        /// Sum of every shard's count, as the response and `affectedRows`.
        ResultType count(const ValidationRule& valid);
        /// Sorted matching keys of every shard as a JSON array (the `limit` smallest ones with a limit).
        ResultType keys(const ValidationRule& valid);

        /// Flushes every shard in parallel, then clears every WAL. If one shard fails, all the WALs are kept, since the
        /// replay needs every shard's records to keep their global order.
        void flush();
        /// flush(), then stops every shard.
        void stop();
        /// Blocks the compaction of every shard for as long as the returned locks live (used while an archive copies the files).
        std::vector<std::shared_lock<std::shared_mutex>> pauseCompaction();
        /// Empties the WAL of every shard.
        void clearAllWAL();
        /// Reads the WAL of every shard, replays all the records through `execCmdFunc` in sequence order, then flushes.
        void replayAllWAL(const std::function<std::string(const std::string&)>& execCmdFunc);
        /// Appends `command` to the WAL of `key`'s shard (buffered, see syncWAL()).
        bool appendToWAL(const std::string& key, const std::string& command, uint64_t* seq = nullptr);
        /// The write order lock of `key`'s shard (see Shard::writeOrderMutex()).
        std::unique_lock<std::mutex> lockWrites(const std::string& key);
        /// The write order locks of every shard, taken in shard order, for a setby / delby logged as one record.
        std::vector<std::unique_lock<std::mutex>> lockAllWrites();
        /// Syncs the WAL of every shard. @return false if one of them failed.
        bool syncWAL();

        /// The sorted matching keys of every shard, at most `valid.limit` of them (the smallest ones).
        std::vector<std::string> firstMatchingKeys(const ValidationRule& valid, bool recordKeysStats = false);
        /// Applies new settings to every shard.
        void reloadSettings(Settings& set);

        const std::vector<std::unique_ptr<Shard>>& getShards() const {
            return this->shards;
        }

    private:
        /// Index of the shard that owns `key`.
        int getShardId(const std::string& key) const;

        Settings settings;
        int numShards;
        std::vector<std::unique_ptr<Shard>> shards;
        std::unique_ptr<ThreadPool> threadPool;

        static constexpr auto hashFunction = std::hash<std::string>{};
    };

} // namespace Zest
