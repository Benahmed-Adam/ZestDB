#include "ShardManager.hpp"
#include "Logger.hpp"
#include "lib/json.hpp"
#include <algorithm>
#include <format>
#include <future>
#include <regex>
#include <thread>

ShardManager::ShardManager(Settings& baseSettings, int numShardsCount)
    : settings(baseSettings)
    , numShards(numShardsCount)
    , threadPool(std::make_unique<ThreadPool>(std::thread::hardware_concurrency()))
{
    for (int i = 0; i < this->numShards; ++i) {
        this->shards.push_back(std::make_unique<Shard>(baseSettings, i));
    }
}

ShardManager::~ShardManager()
{
    this->stop();
}

int ShardManager::getShardId(const std::string& key) const
{
    auto hash = this->hashFunction(key);
    return static_cast<int>(hash % static_cast<unsigned int>(this->numShards));
}

ResultType ShardManager::get(const std::string& key)
{
    auto shardId = this->getShardId(key);
    return this->shards[static_cast<size_t>(shardId)]->get(key);
}

ResultType ShardManager::set(const std::string& key, const std::string& value)
{
    auto shardId = this->getShardId(key);
    return this->shards[static_cast<size_t>(shardId)]->set(key, value);
}

ResultType ShardManager::del(const std::string& key)
{
    auto shardId = this->getShardId(key);
    return this->shards[static_cast<size_t>(shardId)]->del(key);
}

ResultType ShardManager::getBy(ValidationRule& valid)
{
    std::atomic<unsigned int> globalCount(0);
    valid.globalMatchCount = &globalCount;

    std::vector<std::future<ResultType>> futures;

    for (auto& shard : this->shards) {
        Shard* rawShard = shard.get();

        futures.push_back(threadPool->enqueue([rawShard, &valid]() {
            return rawShard->getBy(valid);
        }));
    }

    nlohmann::json mergedArray = nlohmann::json::array();
    long long totalMatches = 0;
    for (auto& f : futures) {
        ResultType r = f.get();
        if (r.code == ResultType::Code::SUCCESS) {
            nlohmann::json shardArray = nlohmann::json::parse(r.response);
            for (auto& item : shardArray) {
                mergedArray.push_back(std::move(item));
            }
            totalMatches += r.affectedRows;
        }
    }
    threadPool->waitAll();
    valid.globalMatchCount = nullptr;
    return { ResultType::Code::SUCCESS, mergedArray.dump(), totalMatches };
}

ResultType ShardManager::setBy(ValidationRule& valid, const std::string& value)
{
    std::atomic<unsigned int> globalCount(0);
    valid.globalMatchCount = &globalCount;

    std::vector<std::future<ResultType>> futures;

    for (auto& shard : this->shards) {
        Shard* rawShard = shard.get();

        futures.push_back(threadPool->enqueue([rawShard, &valid, &value]() {
            return rawShard->setBy(valid, value);
        }));
    }

    long long totalUpdated = 0;
    for (auto& f : futures) {
        ResultType r = f.get();
        if (r.code == ResultType::Code::SUCCESS) {
            totalUpdated += r.affectedRows;
        }
    }
    threadPool->waitAll();
    valid.globalMatchCount = nullptr;
    return { ResultType::Code::SUCCESS, std::format("Value successfully modified for {} entries", totalUpdated), totalUpdated };
}

ResultType ShardManager::delBy(ValidationRule& valid)
{
    std::atomic<unsigned int> globalCount(0);
    valid.globalMatchCount = &globalCount;

    std::vector<std::future<ResultType>> futures;

    for (auto& shard : this->shards) {
        Shard* rawShard = shard.get();

        futures.push_back(threadPool->enqueue([rawShard, &valid]() {
            return rawShard->delBy(valid);
        }));
    }

    long long totalDeleted = 0;
    for (auto& f : futures) {
        ResultType r = f.get();
        if (r.code == ResultType::Code::SUCCESS) {
            totalDeleted += r.affectedRows;
        }
    }
    threadPool->waitAll();
    valid.globalMatchCount = nullptr;
    return { ResultType::Code::SUCCESS, std::format("Successfully deleted {} entries", totalDeleted), totalDeleted };
}

void ShardManager::flush()
{
    for (auto& shard : this->shards) {
        shard->flush();
    }
}

void ShardManager::stop()
{
    for (auto& shard : this->shards) {
        shard->stop();
    }
}

void ShardManager::clearAllWAL()
{
    for (auto& shard : this->shards) {
        shard->getWAL().clear();
    }
}

void ShardManager::replayAllWAL(const std::function<std::string(const std::string&)>& execCmdFunc)
{

    std::vector<WalEntry> allWalEntries;

    for (size_t i = 0; i < this->shards.size(); ++i) {
        WAL& wal = this->shards[i]->getWAL();
        std::vector<WalEntry> cmds = wal.getCmds();

        allWalEntries.insert(
            allWalEntries.end(),
            std::make_move_iterator(cmds.begin()),
            std::make_move_iterator(cmds.end()));
    }

    std::sort(allWalEntries.begin(), allWalEntries.end(), [](const WalEntry& a, const WalEntry& b) { return a.timestamp < b.timestamp; });

    for (const auto& entry : allWalEntries) {
        ZestLog(LogLevel::INFO, std::format("WAL replay command: {}", entry.cmd));
        std::string result = execCmdFunc(entry.cmd);
        ZestLog(LogLevel::INFO, std::format("WAL replay result: {}", result));
    }

    this->flush();
    this->clearAllWAL();
}

void ShardManager::appendToWAL(const std::string& key, const std::string& command)
{
    int shardId = this->getShardId(key);
    auto& shard = this->getShards()[static_cast<size_t>(shardId)];
    shard->getWAL().append(command);
}

void ShardManager::reloadSettings(Settings& set)
{
    this->settings = set;

    for (auto& shard : this->shards) {
        shard->reloadSettings(set);
    }
}