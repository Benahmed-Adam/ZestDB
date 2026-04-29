#include "ShardManager.hpp"
#include <algorithm>
#include <future>
#include <regex>
#include <thread>

ShardManager::ShardManager(const Settings& baseSettings, int numShardsCount)
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
    if (this->settings.isRunning) {
        this->stop();
    }
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

ResultType ShardManager::getBy(const std::regex& reg, unsigned int limit)
{
    std::vector<std::future<ResultType>> futures;

    for (auto& shard : this->shards) {
        futures.push_back(threadPool->enqueue([&]() {
            return shard->getBy(reg, limit);
        }));
    }

    std::string results;
    long long totalMatches = 0;
    for (auto& f : futures) {
        ResultType r = f.get();
        if (r.code == ResultType::Code::SUCCESS) {
            results += r.message;
            totalMatches += r.affectedRows;
        }
    }
    threadPool->waitAll();
    return { ResultType::Code::SUCCESS, results, totalMatches };
}

ResultType ShardManager::setBy(const std::regex& reg, const std::string& value, unsigned int limit)
{
    std::vector<std::future<ResultType>> futures;

    for (auto& shard : this->shards) {
        futures.push_back(threadPool->enqueue([&]() {
            return shard->setBy(reg, value, limit);
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
    return { ResultType::Code::SUCCESS, "Value successfully modified for " + std::to_string(totalUpdated) + " entries", totalUpdated };
}

ResultType ShardManager::delBy(const std::regex& reg, unsigned int limit)
{
    std::vector<std::future<ResultType>> futures;

    for (auto& shard : this->shards) {
        futures.push_back(threadPool->enqueue([&]() {
            return shard->delBy(reg, limit);
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
    return { ResultType::Code::SUCCESS, "Successfully deleted " + std::to_string(totalDeleted) + " entries", totalDeleted };
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