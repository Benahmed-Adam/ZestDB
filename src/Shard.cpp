#include <cstring>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <regex>
#include <sstream>
#include <thread>
#include <unordered_set>

#include "Logger.hpp"
#include "Shard.hpp"

namespace fs = std::filesystem;

Shard::Shard(const Settings& baseSettings, int shardIdNum)
    : settings(baseSettings)
    , shardId(shardIdNum)
    , initialized(false)
    , replaying(false)
{
    boot();

    indexManager = std::make_unique<IndexManager>(this->settings);
    storageManager = std::make_unique<StorageManager>(this->settings);
    cache = std::make_unique<LRUCache>(this->settings.CacheSize);
    compactor = std::make_unique<Compactor>(this->settings.CompactingInterval);

    ZestLog(LogLevel::INFO, "Shard " + std::to_string(shardId) + " initialized successfully");

    std::promise<void> cachePromise;
    std::future<void> cacheFuture = cachePromise.get_future();

    std::thread cacheThread([this, promise = std::move(cachePromise)]() mutable {
        this->fillCache();
        promise.set_value();
    });

    std::thread compactorThread([this]() mutable {
        this->compactor->run(*this->indexManager, *this->storageManager, this->settings.isRunning);
    });

    this->initialized.store(true);
    cacheFuture.wait();
    cacheThread.detach();
    compactorThread.detach();
}

Shard::~Shard()
{
    if (this->settings.isRunning) {
        this->stop();
    }
}

void Shard::boot()
{
    auto shardSettings = this->settings;
    shardSettings.DbPath = this->settings.DbPath / "shards" / std::to_string(shardId);
    shardSettings.IndexPath = shardSettings.DbPath / "INDEX";

    this->settings = shardSettings;

    if (!fs::exists(this->settings.DbPath / "INDEX")) {
        fs::path indexPath = this->settings.DbPath / "INDEX";
        ZestLog(LogLevel::INFO, "Creating INDEX for shard " + std::to_string(shardId) + " at " + indexPath.string());

        if (auto parent = indexPath.parent_path(); !fs::exists(parent)) {
            fs::create_directories(parent);
        }

        std::ofstream index(indexPath);
        if (!index) {
            ZestLog(LogLevel::ERROR, "Failed to create INDEX for shard " + std::to_string(shardId));
            throw std::runtime_error("Failed to create INDEX for shard");
        }
    }

    if (!fs::exists(this->settings.DbPath / "seg")) {
        fs::create_directory(this->settings.DbPath / "seg");
    }
}

void Shard::fillCache()
{
    while (!this->initialized.load()) {
        ZestLog(LogLevel::WARNING, "Shard::fillCache - shard " + std::to_string(shardId) + " not initialized yet, waiting...");
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    ZestLog(LogLevel::INFO, "Filling up the cache for shard " + std::to_string(shardId) + "...");

    std::vector<IndexEntry> entries = this->indexManager->getAll();
    int numKeysInserted = 0;

    std::unordered_set<std::string> seenKeys;
    unsigned int entriesCount = static_cast<unsigned int>(entries.size());
    unsigned int cacheLimit = (this->settings.CacheSize < entriesCount) ? this->settings.CacheSize : entriesCount;

    for (unsigned int i = 0; i < cacheLimit; i++) {
        if (entries[i].segmentId == -1 || entries[i].isTombstone) {
            continue;
        }
        std::string key(entries[i].key);
        if (seenKeys.find(key) != seenKeys.end()) {
            continue;
        }
        seenKeys.insert(key);

        ZestLog(LogLevel::DEBUG, "Shard::fillCache - inserting key: " + key + " in shard " + std::to_string(shardId));
        std::string value = this->storageManager->read(entries[i]);
        this->cache->put(entries[i], value);
        numKeysInserted++;
    }
    ZestLog(LogLevel::INFO, "Shard " + std::to_string(shardId) + " cache filled with " + std::to_string(numKeysInserted) + " keys");
}

ResultType Shard::get(const std::string& key)
{
    std::shared_lock<std::shared_mutex> lock(this->readMtx);
    ZestLog(LogLevel::DEBUG, "Shard::get - shard " + std::to_string(shardId) + " looking for key: " + key);

    CacheEntry cacheEntry = this->cache->get(key);

    if (cacheEntry.index.segmentId != -1 && !cacheEntry.index.isTombstone) {
        ZestLog(LogLevel::DEBUG, "Shard::get - found in cache for shard " + std::to_string(shardId));
        return { ResultType::Code::SUCCESS, cacheEntry.value, 1 };
    }

    ZestLog(LogLevel::DEBUG, "Shard::get - key not in cache, searching index in shard " + std::to_string(shardId));

    IndexEntry entry;
    entry = this->indexManager->search(key);

    if (entry.segmentId != -1 && !entry.isTombstone) {
        ZestLog(LogLevel::DEBUG, "Shard::get - found in segment: " + std::to_string(entry.segmentId));
        std::string value = this->storageManager->read(entry);

        this->cache->put(entry, value);

        return { ResultType::Code::SUCCESS, value, 1 };
    }

    ZestLog(LogLevel::DEBUG, "Shard::get - key not found: " + key + " in shard " + std::to_string(shardId));
    return { ResultType::Code::ERROR, Messages::KEY_NOT_FOUND, 0 };
}

ResultType Shard::set(const std::string& key, const std::string& value)
{
    std::unique_lock<std::shared_mutex> lock(this->readMtx);
    ZestLog(LogLevel::DEBUG, "Shard::set - shard " + std::to_string(shardId) + " key: " + key + ", value size: " + std::to_string(value.size()));

    IndexEntry entry = this->storageManager->append(value);
    ZestLog(LogLevel::DEBUG, "Shard::set - appended to segment: " + std::to_string(entry.segmentId) + ", offset: " + std::to_string(entry.offset));

    memset(entry.key, 0, sizeof(entry.key));
    size_t copySize = (key.size() < sizeof(entry.key) - 1) ? key.size() : sizeof(entry.key) - 1;
    memcpy(entry.key, key.c_str(), copySize);
    entry.key[copySize] = '\0';

    this->indexManager->insert(entry);

    this->cache->put(entry, value);

    ZestLog(LogLevel::DEBUG, "Shard::set - successfully set key: " + key + " in shard " + std::to_string(shardId));
    ResultType result;
    result.code = ResultType::Code::SUCCESS;
    result.message = std::string(Messages::SUCCESS_SET) + key;
    result.affectedRows = 1;
    return result;
}

ResultType Shard::del(const std::string& key)
{
    std::unique_lock<std::shared_mutex> lock(this->readMtx);
    ZestLog(LogLevel::DEBUG, "Shard::del - shard " + std::to_string(shardId) + " deleting key: " + key);

    this->cache->remove(key);
    IndexEntry entry = this->indexManager->search(key);

    if (entry.segmentId != -1 && !entry.isTombstone) {
        entry.isTombstone = true;

        this->indexManager->update(key, entry);
        this->cache->remove(key);

        ZestLog(LogLevel::DEBUG, "Shard::del - successfully deleted key: " + key + " in shard " + std::to_string(shardId));
        ResultType result;
        result.code = ResultType::Code::SUCCESS;
        result.message = std::string(Messages::SUCCESS_DEL) + key;
        result.affectedRows = 1;
        return result;
    }

    ZestLog(LogLevel::DEBUG, "Shard::del - key not found or already deleted: " + key + " in shard " + std::to_string(shardId));
    ResultType result;
    result.code = ResultType::Code::ERROR;
    result.message = std::string(Messages::KEY_NOT_FOUND) + ": " + key;
    result.affectedRows = 0;
    return result;
}

ResultType Shard::getBy(ValidationRule valid)
{
    std::shared_lock<std::shared_mutex> lock(this->readMtx);
    std::vector<IndexEntry> entries;
    entries = this->indexManager->getAll(valid.limit);

    std::ostringstream oss;
    int matchCount = 0;

    for (const IndexEntry& entry : entries) {
        if (valid.limit != UINT_MAX && static_cast<unsigned int>(matchCount) >= valid.limit)
            break;

        if (entry.isTombstone || entry.segmentId == -1) {
            continue;
        }
        std::string key(entry.key);
        if (valid.func(key)) {
            ZestLog(LogLevel::DEBUG, "Shard::getBy - match found: " + key);
            std::string value = this->storageManager->read(entry);
            oss << key << ":" << value << ";";
            matchCount++;
        }
    }

    ResultType result;
    result.code = ResultType::Code::SUCCESS;
    result.message = oss.str();
    result.affectedRows = matchCount;
    return result;
}

ResultType Shard::setBy(ValidationRule valid, const std::string& value)
{
    std::unique_lock<std::shared_mutex> lock(this->readMtx);
    std::vector<IndexEntry> entries;
    entries = this->indexManager->getAll(valid.limit);

    int matchCount = 0;

    for (const IndexEntry& entry : entries) {
        if (valid.limit != UINT_MAX && static_cast<unsigned int>(matchCount) >= valid.limit)
            break;

        if (entry.isTombstone || entry.segmentId == -1) {
            continue;
        }
        std::string key(entry.key);
        if (valid.func(key)) {
            ZestLog(LogLevel::DEBUG, "Shard::setBy - match found: " + key);
            IndexEntry newEntry = this->storageManager->append(value);
            memset(newEntry.key, 0, sizeof(newEntry.key));
            size_t copySize = (key.size() < sizeof(newEntry.key) - 1) ? key.size() : sizeof(newEntry.key) - 1;
            memcpy(newEntry.key, key.c_str(), copySize);
            newEntry.key[copySize] = '\0';

            this->indexManager->insert(newEntry);

            this->cache->put(newEntry, value);

            matchCount++;
        }
    }

    ResultType result;
    result.code = ResultType::Code::SUCCESS;
    result.message = "Value successfully modified for " + std::to_string(matchCount) + " entries";
    result.affectedRows = matchCount;
    return result;
}

ResultType Shard::delBy(ValidationRule valid)
{
    std::unique_lock<std::shared_mutex> lock(this->readMtx);
    std::vector<IndexEntry> entries;
    entries = this->indexManager->getAll(valid.limit);

    int matchCount = 0;

    for (const IndexEntry& entry : entries) {
        if (valid.limit != UINT_MAX && static_cast<unsigned int>(matchCount) >= valid.limit)
            break;

        if (entry.isTombstone || entry.segmentId == -1) {
            continue;
        }
        std::string key(entry.key);
        if (valid.func(key)) {
            ZestLog(LogLevel::DEBUG, "Shard::delBy - match found: " + key);
            IndexEntry tombstoneEntry = entry;
            tombstoneEntry.isTombstone = true;

            this->indexManager->update(key, tombstoneEntry);
            this->cache->remove(key);

            matchCount++;
        }
    }

    ResultType result;
    result.code = ResultType::Code::SUCCESS;
    result.message = "Successfully deleted " + std::to_string(matchCount) + " entries";
    result.affectedRows = matchCount;
    return result;
}

void Shard::flush()
{
    ZestLog(LogLevel::DEBUG, "Flushing shard " + std::to_string(shardId) + "...");
    this->indexManager->flush();
    this->storageManager->flush();
}

void Shard::stop()
{
    ZestLog(LogLevel::INFO, "Exiting shard " + std::to_string(shardId) + "...");
    this->flush();
}