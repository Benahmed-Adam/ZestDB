#include "ShardManager.hpp"
#include <algorithm>
#include <future>
#include <regex>

ShardManager::ShardManager(const Settings& baseSettings, int numShardsCount)
    : settings(baseSettings)
    , numShards(numShardsCount)
{
    for (int i = 0; i < this->numShards; ++i) {
        this->shards.push_back(std::make_unique<Shard>(baseSettings, i));
    }
}

ShardManager::~ShardManager()
{
    stop();
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

ResultType ShardManager::getBy(const std::regex& reg)
{
    std::vector<std::future<ResultType>> futures;

    for (auto& shard : this->shards) {
        futures.push_back(std::async(std::launch::async, [&]() {
            return shard->getBy(reg);
        }));
    }

    std::string results;
    for (auto& f : futures) {
        ResultType r = f.get();
        if (r.code == ResultType::Code::SUCCESS) {
            results += r.message;
        }
    }
    return { ResultType::Code::SUCCESS, results };
}

ResultType ShardManager::setBy(const std::regex& reg, const std::string& value)
{
    std::vector<std::future<ResultType>> futures;

    for (auto& shard : this->shards) {
        futures.push_back(std::async(std::launch::async, [&]() {
            return shard->setBy(reg, value);
        }));
    }

    int totalUpdated = 0;
    for (auto& f : futures) {
        ResultType r = f.get();
        if (r.code == ResultType::Code::SUCCESS) {
            try {
                std::string msg = r.message;
                size_t pos = msg.find("modified for ");
                if (pos != std::string::npos) {
                    std::string numStr = msg.substr(pos + 12);
                    totalUpdated += std::stoi(numStr);
                }
            } catch (...) {
            }
        }
    }
    return { ResultType::Code::SUCCESS, "Value successfully modified for " + std::to_string(totalUpdated) + " entries" };
}

ResultType ShardManager::delBy(const std::regex& reg)
{
    std::vector<std::future<ResultType>> futures;

    for (auto& shard : this->shards) {
        futures.push_back(std::async(std::launch::async, [&]() {
            return shard->delBy(reg);
        }));
    }

    int totalDeleted = 0;
    for (auto& f : futures) {
        ResultType r = f.get();
        if (r.code == ResultType::Code::SUCCESS) {
            try {
                std::string msg = r.message;
                size_t pos = msg.find("Successfully deleted ");
                if (pos != std::string::npos) {
                    std::string numStr = msg.substr(16);
                    totalDeleted += std::stoi(numStr);
                }
            } catch (...) {
            }
        }
    }
    return { ResultType::Code::SUCCESS, "Successfully deleted " + std::to_string(totalDeleted) + " entries" };
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