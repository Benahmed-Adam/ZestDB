#include "Shard.hpp"

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
#include "lib/json.hpp"

namespace Zest {

    namespace fs = std::filesystem;

    Shard::Shard(Settings &baseSettings, int shardIdNum)
        : settings(baseSettings),
          shardId(shardIdNum) {
        this->boot();
    }

    Shard::~Shard() { this->stop(); }

    void Shard::boot() {
        fs::path shardPath = this->settings.DbPath / "shards" / std::to_string(shardId);
        fs::path indexPath = shardPath / "INDEX";
        fs::path walPath = shardPath / "WAL";

        if (!fs::exists(indexPath)) {
            ZestLog(LogLevel::INFO, std::format("Creating INDEX for shard {} at {}", shardId, indexPath.string()));

            if (auto parent = indexPath.parent_path(); !fs::exists(parent)) {
                fs::create_directories(parent);
            }

            std::ofstream idx(indexPath);
            if (!idx) {
                ZestLog(LogLevel::ERROR, std::format("Failed to create INDEX for shard {}", shardId));
                throw std::runtime_error("Failed to create INDEX for shard");
            }
        }

        if (!fs::exists(shardPath / "seg")) {
            fs::create_directory(shardPath / "seg");
        }

        if (!fs::exists(walPath)) {
            ZestLog(LogLevel::INFO, std::format("Creating the WAL at {}", walPath.string()));

            if (auto parent = walPath.parent_path(); !fs::exists(parent)) {
                fs::create_directories(parent);
            }

            std::ofstream walFile(walPath);
            if (!walFile) {
                ZestLog(LogLevel::ERROR, std::format("Failed to create WAL at {}", walPath.string()));
                throw std::runtime_error("Failed to create WAL");
            }
        }

        this->wal = std::make_unique<WAL>(walPath);

        this->settings.DbPath = shardPath;
        this->settings.IndexPath = indexPath;
        this->settings.WalPath = walPath;

        this->indexManager = std::make_unique<IndexManager>(this->settings);
        this->storageManager = std::make_unique<StorageManager>(this->settings);

        this->cache = std::make_unique<LRUCache>(this->settings.CacheSize);

        this->verifyIndexEntries();

        ZestLog(LogLevel::INFO, std::format("Shard {} initialized successfully", this->shardId));

        std::promise<void> cachePromise;
        std::future<void> cacheFuture = cachePromise.get_future();

        std::thread cacheThread([this, promise = std::move(cachePromise)]() mutable {
            this->fillCache();
            promise.set_value();
        });

        this->compactorThread = std::jthread([this](std::stop_token stopToken) {
            std::unique_lock<std::mutex> lock(this->compactorThreadMtx);

            ZestLog(LogLevel::INFO, "Starting the compactor...");

            while (this->settings.isRunning && !stopToken.stop_requested()) {
                this->threadCV.wait_for(lock, stopToken, std::chrono::seconds(this->settings.CompactingInterval), [this, stopToken] { return stopToken.stop_requested() || !this->settings.isRunning; });

                std::vector<IndexEntry> entries = indexManager->compact();

                if (entries.empty()) {
                    ZestLog(LogLevel::DEBUG, "Compactor - index is empty, skipping segment cleanup");
                    continue;
                }

                if (stopToken.stop_requested() || !this->settings.isRunning) {
                    break;
                }

                std::vector<uint32_t> usedSegmentIds;
                for (const auto &entry : entries) {
                    if (!entry.isTombstone && entry.segmentId != INVALID_SEGMENT_ID) {
                        if (std::find(usedSegmentIds.begin(), usedSegmentIds.end(), entry.segmentId) == usedSegmentIds.end()) {
                            usedSegmentIds.push_back(entry.segmentId);
                        }
                    }
                }

                if (!usedSegmentIds.empty()) {
                    storageManager->removeUnusedSegments(usedSegmentIds);
                }

                ZestLog(LogLevel::DEBUG, "Compactor - compaction done, waiting...");
            }
            ZestLog(LogLevel::INFO, "Stopping the compactor...");
        });

        cacheFuture.wait();

        if (cacheThread.joinable()) {
            cacheThread.join();
        }
    }

    void Shard::fillCache() {
        std::vector<IndexEntry> entries = this->indexManager->getAll();
        int numKeysInserted = 0;

        std::unordered_set<std::string> seenKeys;
        unsigned int entriesCount = static_cast<unsigned int>(entries.size());
        unsigned int cacheLimit = (this->settings.CacheSize < entriesCount) ? this->settings.CacheSize : entriesCount;

        for (unsigned int i = 0; i < cacheLimit; i++) {
            if (entries[i].segmentId == INVALID_SEGMENT_ID || entries[i].isTombstone) {
                continue;
            }
            if (seenKeys.find(entries[i].key) != seenKeys.end()) {
                continue;
            }
            seenKeys.insert(entries[i].key);

            std::string value = this->storageManager->read(entries[i]);
            this->cache->put(entries[i], value);
            numKeysInserted++;
        }
        ZestLog(LogLevel::INFO, std::format("Shard {} cache filled with {} keys", shardId, numKeysInserted));
    }

    ResultType Shard::get(const std::string &key) {
        auto start = std::chrono::high_resolution_clock::now();
        std::shared_lock<std::shared_mutex> lock(this->readMtx);

        CacheEntry cacheEntry = this->cache->get(key);

        bool fromCache = (cacheEntry.index.segmentId != INVALID_SEGMENT_ID && !cacheEntry.index.isTombstone);

        if (fromCache) {
            auto end = std::chrono::high_resolution_clock::now();
            double latency = std::chrono::duration<double, std::milli>(end - start).count();
            this->perfMonitor.addGetStats(false, latency);
            return { ResultType::Code::SUCCESS, cacheEntry.value, 1 };
        }

        IndexEntry entry;
        entry = this->indexManager->search(key);

        if (entry.segmentId != INVALID_SEGMENT_ID && !entry.isTombstone) {
            std::string value = this->storageManager->read(entry);

            this->cache->put(entry, value);

            auto end = std::chrono::high_resolution_clock::now();
            double latency = std::chrono::duration<double, std::milli>(end - start).count();
            this->perfMonitor.addGetStats(true, latency);
            return { ResultType::Code::SUCCESS, value, 1 };
        }

        auto end = std::chrono::high_resolution_clock::now();
        double latency = std::chrono::duration<double, std::milli>(end - start).count();
        this->perfMonitor.addGetStats(true, latency);
        return { ResultType::Code::ERROR, Messages::KEY_NOT_FOUND, 0 };
    }

    ResultType Shard::set(const std::string &key, const std::string &value) {
        auto start = std::chrono::high_resolution_clock::now();
        std::unique_lock<std::shared_mutex> lock(this->readMtx);

        IndexEntry entry = this->storageManager->append(value);

        if (entry.segmentId == INVALID_SEGMENT_ID) {
            ZestLog(LogLevel::ERROR, std::format("Shard::set - FAILED to write key: {} in shard {} "
                                                 "- segment full",
                                                 key, shardId));
            auto end = std::chrono::high_resolution_clock::now();
            double latency = std::chrono::duration<double, std::milli>(end - start).count();
            this->perfMonitor.addSetStats(false, latency);
            return { ResultType::Code::ERROR, "Failed to write: segment full", 0 };
        }

        entry.key = key;

        this->indexManager->insert(entry);

        this->cache->put(entry, value);

        auto end = std::chrono::high_resolution_clock::now();
        double latency = std::chrono::duration<double, std::milli>(end - start).count();
        this->perfMonitor.addSetStats(false, latency);
        return { ResultType::Code::SUCCESS, Messages::SUCCESS_SET, 1 };
    }

    ResultType Shard::del(const std::string &key) {
        auto start = std::chrono::high_resolution_clock::now();
        std::unique_lock<std::shared_mutex> lock(this->readMtx);

        this->cache->remove(key);
        IndexEntry entry = this->indexManager->search(key);

        if (entry.segmentId != INVALID_SEGMENT_ID && !entry.isTombstone) {
            entry.isTombstone = true;

            this->indexManager->update(key, entry);
            this->cache->remove(key);

            auto end = std::chrono::high_resolution_clock::now();
            double latency = std::chrono::duration<double, std::milli>(end - start).count();
            this->perfMonitor.addDelStats(false, latency);
            return { ResultType::Code::SUCCESS, Messages::SUCCESS_DEL, 1 };
        }

        auto end = std::chrono::high_resolution_clock::now();
        double latency = std::chrono::duration<double, std::milli>(end - start).count();
        this->perfMonitor.addDelStats(false, latency);
        return { ResultType::Code::ERROR, Messages::KEY_NOT_FOUND, 0 };
    }

    ResultType Shard::getBy(ValidationRule &valid) {
        auto start = std::chrono::high_resolution_clock::now();
        std::shared_lock<std::shared_mutex> lock(this->readMtx);
        std::vector<IndexEntry> entries;
        entries = this->indexManager->getAll(valid.limit);

        nlohmann::json resultArray = nlohmann::json::array();
        int matchCount = 0;

        for (const IndexEntry &entry : entries) {
            if (valid.limit != UINT_MAX && valid.globalMatchCount && valid.globalMatchCount->load() >= valid.limit)
                break;

            if (entry.isTombstone || entry.segmentId == INVALID_SEGMENT_ID) {
                continue;
            }
            if (valid.func(entry.key)) {
                std::string value = this->storageManager->read(entry);
                resultArray.push_back(nlohmann::json::object({ { entry.key, value } }));
                matchCount++;
                if (valid.globalMatchCount) {
                    valid.globalMatchCount->fetch_add(1);
                }
                if (valid.limit != UINT_MAX && valid.globalMatchCount && valid.globalMatchCount->load() >= valid.limit)
                    break;
            }
        }

        auto end = std::chrono::high_resolution_clock::now();
        double latency = std::chrono::duration<double, std::milli>(end - start).count();
        this->perfMonitor.addGetByStats(false, latency);

        return { ResultType::Code::SUCCESS, resultArray.dump(), matchCount };
    }

    ResultType Shard::setBy(ValidationRule &valid, const std::string &value) {
        auto start = std::chrono::high_resolution_clock::now();
        std::unique_lock<std::shared_mutex> lock(this->readMtx);
        std::vector<IndexEntry> entries;
        entries = this->indexManager->getAll(valid.limit);

        int matchCount = 0;

        for (const IndexEntry &entry : entries) {
            if (valid.limit != UINT_MAX && valid.globalMatchCount && valid.globalMatchCount->load() >= valid.limit)
                break;

            if (entry.isTombstone || entry.segmentId == INVALID_SEGMENT_ID) {
                continue;
            }
            if (valid.func(entry.key)) {
                IndexEntry newEntry = this->storageManager->append(value);

                if (newEntry.segmentId == INVALID_SEGMENT_ID) {
                    ZestLog(LogLevel::ERROR, std::format("Shard::setBy - FAILED to write for key: "
                                                         "{} - segment full",
                                                         entry.key));
                    continue;
                }

                newEntry.key = entry.key;

                this->indexManager->insert(newEntry);

                this->cache->put(newEntry, value);

                matchCount++;
                if (valid.globalMatchCount) {
                    valid.globalMatchCount->fetch_add(1);
                }
                if (valid.limit != UINT_MAX && valid.globalMatchCount && valid.globalMatchCount->load() >= valid.limit)
                    break;
            }
        }

        auto end = std::chrono::high_resolution_clock::now();
        double latency = std::chrono::duration<double, std::milli>(end - start).count();
        this->perfMonitor.addSetByStats(false, latency);

        return { ResultType::Code::SUCCESS, std::format("Value successfully modified for {} entries", matchCount), matchCount };
    }

    ResultType Shard::delBy(ValidationRule &valid) {
        auto start = std::chrono::high_resolution_clock::now();
        std::unique_lock<std::shared_mutex> lock(this->readMtx);
        std::vector<IndexEntry> entries;
        entries = this->indexManager->getAll(valid.limit);

        int matchCount = 0;

        for (const IndexEntry &entry : entries) {
            if (valid.limit != UINT_MAX && valid.globalMatchCount && valid.globalMatchCount->load() >= valid.limit)
                break;

            if (entry.isTombstone || entry.segmentId == INVALID_SEGMENT_ID) {
                continue;
            }
            if (valid.func(entry.key)) {
                IndexEntry tombstoneEntry = entry;
                tombstoneEntry.isTombstone = true;

                this->indexManager->update(entry.key, tombstoneEntry);
                this->cache->remove(entry.key);

                matchCount++;
                if (valid.globalMatchCount) {
                    valid.globalMatchCount->fetch_add(1);
                }
                if (valid.limit != UINT_MAX && valid.globalMatchCount && valid.globalMatchCount->load() >= valid.limit)
                    break;
            }
        }

        auto end = std::chrono::high_resolution_clock::now();
        double latency = std::chrono::duration<double, std::milli>(end - start).count();
        this->perfMonitor.addDelByStats(false, latency);

        return { ResultType::Code::SUCCESS, std::format("Successfully deleted {} entries", matchCount), matchCount };
    }

    void Shard::flush() {
        this->indexManager->flush();
        this->storageManager->flush();
        this->wal->clear();
    }

    void Shard::stop() {
        if (!this->isStopped.load()) {
            ZestLog(LogLevel::INFO, std::format("Exiting shard {}...", shardId));

            this->settings.isRunning = false;
            this->compactorThread.request_stop();
            this->threadCV.notify_all();

            this->flush();

            this->isStopped.store(true);
        }
    }

    void Shard::verifyIndexEntries() {
        ZestLog(LogLevel::INFO, std::format("Shard {} - Verifying index entries...", this->shardId));

        std::vector<IndexEntry> entries = this->indexManager->getAll();
        int verifiedCount = 0;
        int removedCount = 0;

        for (const auto &entry : entries) {
            if (entry.isTombstone || entry.segmentId == INVALID_SEGMENT_ID) {
                continue;
            }

            std::string storedValue = this->storageManager->read(entry);

            if (storedValue.empty()) {
                ZestLog(LogLevel::WARNING, std::format("Shard {} - Removing invalid index entry for key: {}", this->shardId, entry.key));

                IndexEntry tombstoneEntry = entry;
                tombstoneEntry.isTombstone = true;
                this->indexManager->update(entry.key, tombstoneEntry);
                removedCount++;
            } else {
                verifiedCount++;
            }
        }

        ZestLog(LogLevel::INFO, std::format("Shard {} - Index verification complete: {} valid, {} removed", this->shardId, verifiedCount, removedCount));
    }

    void Shard::reloadSettings(Settings &set) {
        fs::path shardPath = set.DbPath / "shards" / std::to_string(shardId);
        fs::path indexPath = shardPath / "INDEX";
        fs::path walPath = shardPath / "WAL";

        this->settings = set;

        this->settings.DbPath = shardPath;
        this->settings.IndexPath = indexPath;
        this->settings.WalPath = walPath;
    }

} // namespace Zest
