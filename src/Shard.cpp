#include <cstring>
#include <filesystem>
#include <format>
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
    , stopRequested(false)
    , stopped(false)
{
    this->boot();

    this->indexManager = std::make_unique<IndexManager>(this->settings);
    this->storageManager = std::make_unique<StorageManager>(this->settings);
    this->cache = std::make_unique<LRUCache>(this->settings.CacheSize);
    this->compactor = std::make_unique<Compactor>(this->settings.CompactingInterval);

    this->verifyIndexEntries();

    ZestLog(LogLevel::INFO, std::format("Shard {} initialized successfully", this->shardId));

    std::promise<void> cachePromise;
    std::future<void> cacheFuture = cachePromise.get_future();

    std::thread cacheThread([this, promise = std::move(cachePromise)]() mutable {
        this->fillCache();
        promise.set_value();
    });

    std::thread compactorThread([this]() mutable {
        this->compactor->run(*this->indexManager, *this->storageManager, this->stopRequested);
    });

    this->initialized.store(true);
    cacheFuture.wait();
    cacheThread.detach();
    compactorThread.detach();
}

Shard::~Shard()
{
    this->stop();
}

void Shard::boot()
{
    auto shardSettings = this->settings;
    shardSettings.DbPath = this->settings.DbPath / "shards" / std::to_string(shardId);
    shardSettings.IndexPath = shardSettings.DbPath / "INDEX";

    this->settings = shardSettings;

    if (!fs::exists(this->settings.DbPath / "INDEX")) {
        fs::path indexPath = this->settings.DbPath / "INDEX";
        ZestLog(LogLevel::INFO, std::format("Creating INDEX for shard {} at {}", shardId, indexPath.string()));

        if (auto parent = indexPath.parent_path(); !fs::exists(parent)) {
            fs::create_directories(parent);
        }

        std::ofstream index(indexPath);
        if (!index) {
            ZestLog(LogLevel::ERROR, std::format("Failed to create INDEX for shard {}", shardId));
            throw std::runtime_error("Failed to create INDEX for shard");
        }
    }

    if (!fs::exists(this->settings.DbPath / "seg")) {
        fs::create_directory(this->settings.DbPath / "seg");
    }

    this->settings.WalPath = this->settings.DbPath / "WAL";

    if (!fs::exists(this->settings.DbPath / "WAL")) {
        fs::path WalPath = this->settings.DbPath / "WAL";
        ZestLog(LogLevel::INFO, std::format("Creating the WAL at {}", WalPath.string()));

        if (auto parent = WalPath.parent_path(); !fs::exists(parent)) {
            fs::create_directories(parent);
        }

        std::ofstream index(WalPath);
        if (!index) {
            ZestLog(LogLevel::ERROR, std::format("Failed to create WAL at {}", WalPath.string()));
            throw std::runtime_error("Failed to create WAL");
        }
        this->settings.WalPath = WalPath;
    } else {
        this->settings.WalPath = this->settings.DbPath / "WAL";
    }

    this->wal = std::make_unique<WAL>(this->settings);
}

void Shard::fillCache()
{
    while (!this->initialized.load()) {
        ZestLog(LogLevel::WARNING, std::format("Shard::fillCache - shard {} not initialized yet, waiting...", shardId));
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    ZestLog(LogLevel::INFO, std::format("Filling up the cache for shard {}...", shardId));

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

        ZestLog(LogLevel::DEBUG, std::format("Shard::fillCache - inserting key: {} in shard {}", key, shardId));
        std::string value = this->storageManager->read(entries[i]);
        this->cache->put(entries[i], value);
        numKeysInserted++;
    }
    ZestLog(LogLevel::INFO, std::format("Shard {} cache filled with {} keys", shardId, numKeysInserted));
}

ResultType Shard::get(const std::string& key)
{
    auto start = std::chrono::high_resolution_clock::now();
    std::shared_lock<std::shared_mutex> lock(this->readMtx);
    ZestLog(LogLevel::DEBUG, std::format("Shard::get - shard {} looking for key: {}", shardId, key));

    CacheEntry cacheEntry = this->cache->get(key);

    bool fromCache = (cacheEntry.index.segmentId != -1 && !cacheEntry.index.isTombstone);

    if (fromCache) {
        ZestLog(LogLevel::DEBUG, std::format("Shard::get - found in cache for shard {}", shardId));
        auto end = std::chrono::high_resolution_clock::now();
        double latency = std::chrono::duration<double, std::milli>(end - start).count();
        this->perfMonitor.addGetStats(true, false, latency);
        return { ResultType::Code::SUCCESS, cacheEntry.value, 1 };
    }

    ZestLog(LogLevel::DEBUG, std::format("Shard::get - key not in cache, searching index in shard {}", shardId));

    IndexEntry entry;
    entry = this->indexManager->search(key);

    if (entry.segmentId != -1 && !entry.isTombstone) {
        ZestLog(LogLevel::DEBUG, std::format("Shard::get - found in segment: {}", entry.segmentId));
        std::string value = this->storageManager->read(entry);

        this->cache->put(entry, value);

        auto end = std::chrono::high_resolution_clock::now();
        double latency = std::chrono::duration<double, std::milli>(end - start).count();
        this->perfMonitor.addGetStats(true, true, latency);
        return { ResultType::Code::SUCCESS, value, 1 };
    }

    ZestLog(LogLevel::DEBUG, std::format("Shard::get - key not found: {} in shard {}", key, shardId));
    auto end = std::chrono::high_resolution_clock::now();
    double latency = std::chrono::duration<double, std::milli>(end - start).count();
    this->perfMonitor.addGetStats(false, true, latency);
    return { ResultType::Code::ERROR, Messages::KEY_NOT_FOUND, 0 };
}

ResultType Shard::set(const std::string& key, const std::string& value)
{
    auto start = std::chrono::high_resolution_clock::now();
    std::unique_lock<std::shared_mutex> lock(this->readMtx);
    ZestLog(LogLevel::DEBUG, std::format("Shard::set - shard {} key: {}, value size: {}", shardId, key, value.size()));

    IndexEntry entry = this->storageManager->append(value);
    ZestLog(LogLevel::DEBUG, std::format("Shard::set - appended to segment: {}, offset: {}", entry.segmentId, entry.offset));

    memset(entry.key, 0, sizeof(entry.key));
    size_t copySize = (key.size() < sizeof(entry.key) - 1) ? key.size() : sizeof(entry.key) - 1;
    memcpy(entry.key, key.c_str(), copySize);
    entry.key[copySize] = '\0';

    this->indexManager->insert(entry);

    this->cache->put(entry, value);

    ZestLog(LogLevel::DEBUG, std::format("Shard::set - successfully set key: {} in shard {}", key, shardId));
    auto end = std::chrono::high_resolution_clock::now();
    double latency = std::chrono::duration<double, std::milli>(end - start).count();
    this->perfMonitor.addSetStats(true, false, latency);
    ResultType result;
    result.code = ResultType::Code::SUCCESS;
    result.message = std::string(Messages::SUCCESS_SET) + key;
    result.affectedRows = 1;
    return result;
}

ResultType Shard::del(const std::string& key)
{
    auto start = std::chrono::high_resolution_clock::now();
    std::unique_lock<std::shared_mutex> lock(this->readMtx);
    ZestLog(LogLevel::DEBUG, std::format("Shard::del - shard {} deleting key: {}", shardId, key));

    this->cache->remove(key);
    IndexEntry entry = this->indexManager->search(key);

    if (entry.segmentId != -1 && !entry.isTombstone) {
        entry.isTombstone = true;

        this->indexManager->update(key, entry);
        this->cache->remove(key);

        ZestLog(LogLevel::DEBUG, std::format("Shard::del - successfully deleted key: {} in shard {}", key, shardId));
        auto end = std::chrono::high_resolution_clock::now();
        double latency = std::chrono::duration<double, std::milli>(end - start).count();
        this->perfMonitor.addDelStats(true, false, latency);
        ResultType result;
        result.code = ResultType::Code::SUCCESS;
        result.message = std::string(Messages::SUCCESS_DEL) + key;
        result.affectedRows = 1;
        return result;
    }

    ZestLog(LogLevel::DEBUG, std::format("Shard::del - key not found or already deleted: {} in shard {}", key, shardId));
    auto end = std::chrono::high_resolution_clock::now();
    double latency = std::chrono::duration<double, std::milli>(end - start).count();
    this->perfMonitor.addDelStats(false, false, latency);
    ResultType result;
    result.code = ResultType::Code::ERROR;
    result.message = std::format("{}: {}", Messages::KEY_NOT_FOUND, key);
    result.affectedRows = 0;
    return result;
}

ResultType Shard::getBy(ValidationRule valid)
{
    auto start = std::chrono::high_resolution_clock::now();
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
            ZestLog(LogLevel::DEBUG, std::format("Shard::getBy - match found: {}", key));
            std::string value = this->storageManager->read(entry);
            oss << key << ":" << value << ";";
            matchCount++;
        }
    }

    auto end = std::chrono::high_resolution_clock::now();
    double latency = std::chrono::duration<double, std::milli>(end - start).count();
    this->perfMonitor.addGetByStats(matchCount > 0, false, latency);

    ResultType result;
    result.code = ResultType::Code::SUCCESS;
    result.message = oss.str();
    result.affectedRows = matchCount;
    return result;
}

ResultType Shard::setBy(ValidationRule valid, const std::string& value)
{
    auto start = std::chrono::high_resolution_clock::now();
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
            ZestLog(LogLevel::DEBUG, std::format("Shard::setBy - match found: {}", key));
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

    auto end = std::chrono::high_resolution_clock::now();
    double latency = std::chrono::duration<double, std::milli>(end - start).count();
    this->perfMonitor.addSetByStats(matchCount > 0, false, latency);

    ResultType result;
    result.code = ResultType::Code::SUCCESS;
    result.message = std::format("Value successfully modified for {} entries", matchCount);
    result.affectedRows = matchCount;
    return result;
}

ResultType Shard::delBy(ValidationRule valid)
{
    auto start = std::chrono::high_resolution_clock::now();
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
            ZestLog(LogLevel::DEBUG, std::format("Shard::delBy - match found: {}", key));
            IndexEntry tombstoneEntry = entry;
            tombstoneEntry.isTombstone = true;

            this->indexManager->update(key, tombstoneEntry);
            this->cache->remove(key);

            matchCount++;
        }
    }

    auto end = std::chrono::high_resolution_clock::now();
    double latency = std::chrono::duration<double, std::milli>(end - start).count();
    this->perfMonitor.addDelByStats(matchCount > 0, false, latency);

    ResultType result;
    result.code = ResultType::Code::SUCCESS;
    result.message = std::format("Successfully deleted {} entries", matchCount);
    result.affectedRows = matchCount;
    return result;
}

void Shard::flush()
{
    ZestLog(LogLevel::DEBUG, std::format("Flushing shard {}...", shardId));
    this->indexManager->flush();
    this->storageManager->flush();
    this->wal->clear();
}

void Shard::stop()
{
    bool expected = false;
    if (!this->stopped.compare_exchange_strong(expected, true)) {
        return;
    }

    ZestLog(LogLevel::INFO, std::format("Exiting shard {}...", shardId));

    this->stopRequested.store(true);

    this->flush();
}

void Shard::verifyIndexEntries()
{
    ZestLog(LogLevel::INFO, std::format("Shard {} - Verifying index entries...", this->shardId));

    std::vector<IndexEntry> entries = this->indexManager->getAll();
    int verifiedCount = 0;
    int removedCount = 0;

    for (const auto& entry : entries) {
        if (entry.isTombstone || entry.segmentId == -1) {
            continue;
        }

        std::string key(entry.key);

        std::string storedValue = this->storageManager->read(entry);

        if (storedValue.empty()) {
            ZestLog(LogLevel::WARNING, std::format("Shard {} - Removing invalid index entry for key: {}", this->shardId, key));

            IndexEntry tombstoneEntry = entry;
            tombstoneEntry.isTombstone = true;
            this->indexManager->update(key, tombstoneEntry);
            removedCount++;
        } else {
            verifiedCount++;
        }
    }

    ZestLog(LogLevel::INFO, std::format("Shard {} - Index verification complete: {} valid, {} removed", this->shardId, verifiedCount, removedCount));
}
