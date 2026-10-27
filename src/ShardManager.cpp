#include "ShardManager.hpp"

#include <algorithm>
#include <format>
#include <future>
#include <regex>
#include <thread>

#include "Logger.hpp"
#include "lib/json.hpp"

namespace Zest {

    ShardManager::ShardManager(Settings& baseSettings, int numShardsCount)
        : settings(baseSettings),
          numShards(numShardsCount),
          threadPool(std::make_unique<ThreadPool>(std::thread::hardware_concurrency() > 0 ? std::thread::hardware_concurrency() : 8)) {
        for (int i = 0; i < this->numShards; ++i) {
            this->shards.push_back(std::make_unique<Shard>(baseSettings, i));
        }
    }

    ShardManager::~ShardManager() {
        this->stop();
    }

    int ShardManager::getShardId(const std::string& key) const {
        auto hash = this->hashFunction(key);
        return static_cast<int>(hash % static_cast<unsigned int>(this->numShards));
    }

    ResultType ShardManager::get(const std::string& key) {
        int shardId = this->getShardId(key);
        return this->shards[static_cast<size_t>(shardId)]->get(key);
    }

    ResultType ShardManager::set(const std::string& key, const std::string& value) {
        int shardId = this->getShardId(key);
        return this->shards[static_cast<size_t>(shardId)]->set(key, value);
    }

    ResultType ShardManager::del(const std::string& key) {
        int shardId = this->getShardId(key);
        return this->shards[static_cast<size_t>(shardId)]->del(key);
    }

    ResultType ShardManager::getBy(ValidationRule& valid) {
        std::atomic<unsigned int> globalCount(0);
        valid.globalMatchCount = &globalCount;

        std::vector<std::future<ResultType>> futures;

        for (auto& shard : this->shards) {
            Shard* rawShard = shard.get();

            futures.push_back(threadPool->enqueue([rawShard, &valid]() { return rawShard->getBy(valid); }));
        }

        // Each shard returns a JSON array and the shared counter already caps the total at the limit: join the arrays as text instead of parsing them again
        std::string merged = "[";
        long long totalMatches = 0;
        for (auto& f : futures) {
            ResultType r = f.get();
            if (r.code == ResultType::Code::Success && r.response.size() > 2) {
                if (merged.size() > 1) {
                    merged += ',';
                }
                merged.append(r.response, 1, r.response.size() - 2);
                totalMatches += r.affectedRows;
            }
        }
        merged += ']';
        valid.globalMatchCount = nullptr;
        return { ResultType::Code::Success, std::move(merged), totalMatches };
    }

    std::vector<std::string> ShardManager::firstMatchingKeys(const ValidationRule& valid) {
        std::vector<std::future<std::vector<std::string>>> perShard;
        for (auto& shard : this->shards) {
            Shard* rawShard = shard.get();
            perShard.push_back(this->threadPool->enqueue([rawShard, &valid]() { return rawShard->matchingKeys(valid); }));
        }
        std::vector<std::string> keys;
        for (auto& f : perShard) {
            std::vector<std::string> shardKeys = f.get();
            keys.insert(keys.end(), std::make_move_iterator(shardKeys.begin()), std::make_move_iterator(shardKeys.end()));
        }
        if (keys.size() > valid.limit) {
            std::nth_element(keys.begin(), keys.begin() + valid.limit, keys.end());
            keys.resize(valid.limit);
        }
        std::sort(keys.begin(), keys.end());
        return keys;
    }

    ResultType ShardManager::setBy(ValidationRule& valid, const std::string& value) {
        if (valid.limit != UINT_MAX) {
            long long updated = 0;
            for (const auto& key : this->firstMatchingKeys(valid)) {
                updated += this->set(key, value).code == ResultType::Code::Success;
            }
            return { ResultType::Code::Success, std::format("Value successfully modified for {} entries", updated), updated };
        }

        std::atomic<unsigned int> globalCount(0);
        valid.globalMatchCount = &globalCount;

        std::vector<std::future<ResultType>> futures;

        for (auto& shard : this->shards) {
            Shard* rawShard = shard.get();

            futures.push_back(threadPool->enqueue([rawShard, &valid, &value]() { return rawShard->setBy(valid, value); }));
        }

        long long totalUpdated = 0;
        for (auto& f : futures) {
            ResultType r = f.get();
            if (r.code == ResultType::Code::Success) {
                totalUpdated += r.affectedRows;
            }
        }
        valid.globalMatchCount = nullptr;
        if (valid.limit != UINT_MAX && totalUpdated > static_cast<long long>(valid.limit)) {
            totalUpdated = static_cast<long long>(valid.limit);
        }
        return { ResultType::Code::Success, std::format("Value successfully modified for {} entries", totalUpdated), totalUpdated };
    }

    ResultType ShardManager::delBy(ValidationRule& valid) {
        if (valid.limit != UINT_MAX) {
            long long deleted = 0;
            for (const auto& key : this->firstMatchingKeys(valid)) {
                deleted += this->del(key).code == ResultType::Code::Success;
            }
            return { ResultType::Code::Success, std::format("Successfully deleted {} entries", deleted), deleted };
        }

        std::atomic<unsigned int> globalCount(0);
        valid.globalMatchCount = &globalCount;

        std::vector<std::future<ResultType>> futures;

        for (auto& shard : this->shards) {
            Shard* rawShard = shard.get();

            futures.push_back(threadPool->enqueue([rawShard, &valid]() { return rawShard->delBy(valid); }));
        }

        long long totalDeleted = 0;
        for (auto& f : futures) {
            ResultType r = f.get();
            if (r.code == ResultType::Code::Success) {
                totalDeleted += r.affectedRows;
            }
        }
        valid.globalMatchCount = nullptr;
        if (valid.limit != UINT_MAX && totalDeleted > static_cast<long long>(valid.limit)) {
            totalDeleted = static_cast<long long>(valid.limit);
        }
        return { ResultType::Code::Success, std::format("Successfully deleted {} entries", totalDeleted), totalDeleted };
    }

    void ShardManager::flush() {
        // Shards flush in parallel: callers hold the global write lock, so the stall is the slowest shard's fsync, not their sum
        std::vector<std::future<bool>> results;
        for (auto& shard : this->shards) {
            Shard* rawShard = shard.get();
            results.push_back(this->threadPool->enqueue([rawShard]() { return rawShard->flushData(); }));
        }
        bool ok = true;
        for (auto& result : results) {
            ok = result.get() && ok;
        }
        if (!ok) {
            ZestLog(LogLevel::Error, "ShardManager - a shard failed to flush, keeping every WAL");
            return;
        }
        this->clearAllWAL();
    }

    void ShardManager::stop() {
        this->flush();
        for (auto& shard : this->shards) {
            shard->stop();
        }
    }

    std::vector<std::shared_lock<std::shared_mutex>> ShardManager::lockAllShards() {
        std::vector<std::shared_lock<std::shared_mutex>> locks;
        for (auto& shard : this->shards) {
            locks.emplace_back(shard->mutex());
        }
        return locks;
    }

    void ShardManager::clearAllWAL() {
        for (auto& shard : this->shards) {
            shard->getWAL().clear();
        }
    }

    void ShardManager::replayAllWAL(const std::function<std::string(const std::string&)>& execCmdFunc) {

        std::vector<WalEntry> allWalEntries;

        for (size_t i = 0; i < this->shards.size(); ++i) {
            WAL& wal = this->shards[i]->getWAL();
            std::vector<WalEntry> cmds = wal.getCmds();

            allWalEntries.insert(allWalEntries.end(), std::make_move_iterator(cmds.begin()), std::make_move_iterator(cmds.end()));
        }

        std::stable_sort(allWalEntries.begin(), allWalEntries.end(), [](const WalEntry& a, const WalEntry& b) { return a.seq < b.seq; });
        if (!allWalEntries.empty()) {
            WAL::advanceSequence(allWalEntries.back().seq + 1);
        }

        for (const auto& entry : allWalEntries) {
            ZestLog(LogLevel::Debug, std::format("WAL replay command: {}", entry.cmd));
            std::string result = execCmdFunc(entry.cmd);
            ZestLog(LogLevel::Debug, std::format("WAL replay result: {}", result));
        }

        this->flush();
    }

    bool ShardManager::syncWAL() {
        bool ok = true;
        for (auto& shard : this->shards) {
            ok = shard->getWAL().sync() && ok;
        }
        return ok;
    }

    bool ShardManager::appendToWAL(const std::string& key, const std::string& command) {
        int shardId = this->getShardId(key);
        auto& shard = this->getShards()[static_cast<size_t>(shardId)];
        return shard->getWAL().append(command);
    }

    void ShardManager::reloadSettings(Settings& set) {
        this->settings = set;

        for (auto& shard : this->shards) {
            shard->reloadSettings(set);
        }
    }

} // namespace Zest
