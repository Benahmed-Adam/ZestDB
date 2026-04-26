#include "ShardManager.hpp"
#include <algorithm>
#include <future>
#include <regex>

ShardManager::ShardManager(const Settings& baseSettings, int numShardsCount)
    : settings(baseSettings)
    , numShards(numShardsCount)
{
    for (int i = 0; i < numShards; ++i) {
        shards.push_back(std::make_unique<Shard>(baseSettings, i));
    }
}

ShardManager::~ShardManager()
{
    stop();
}

int ShardManager::getShardId(const std::string& key) const
{
    auto hash = hashFunction(key);
    return static_cast<int>(hash % static_cast<unsigned int>(numShards));
}

ResultType ShardManager::get(const std::string& key)
{
    auto shardId = getShardId(key);
    return shards[static_cast<size_t>(shardId)]->get(key);
}

ResultType ShardManager::set(const std::string& key, const std::string& value)
{
    auto shardId = getShardId(key);
    return shards[static_cast<size_t>(shardId)]->set(key, value);
}

ResultType ShardManager::del(const std::string& key)
{
    auto shardId = getShardId(key);
    return shards[static_cast<size_t>(shardId)]->del(key);
}

ResultType ShardManager::getBy(const std::string& pattern)
{
    std::vector<std::future<ResultType>> futures;

    for (auto& shard : shards) {
        futures.push_back(std::async(std::launch::async, [&]() {
            return shard->getBy(pattern);
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

ResultType ShardManager::setBy(const std::string& pattern, const std::string& value)
{
    try {
        std::regex re(pattern);
    } catch (const std::regex_error& e) {
        return { ResultType::Code::ERROR, Messages::INVALID_REGEX };
    }

    std::vector<std::future<ResultType>> futures;

    for (auto& shard : shards) {
        futures.push_back(std::async(std::launch::async, [&]() {
            return shard->setBy(pattern, value);
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

ResultType ShardManager::delBy(const std::string& pattern)
{
    try {
        std::regex re(pattern);
    } catch (const std::regex_error& e) {
        return { ResultType::Code::ERROR, Messages::INVALID_REGEX };
    }

    std::vector<std::future<ResultType>> futures;

    for (auto& shard : shards) {
        futures.push_back(std::async(std::launch::async, [&]() {
            return shard->delBy(pattern);
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
    for (auto& shard : shards) {
        shard->flush();
    }
}

void ShardManager::stop()
{
    for (auto& shard : shards) {
        shard->stop();
    }
}