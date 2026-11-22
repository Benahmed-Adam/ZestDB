#include "Shard.hpp"

#include <algorithm>
#include <charconv>
#include <filesystem>
#include <format>
#include <fstream>
#include <future>
#include <iostream>
#include <regex>
#include <sstream>
#include <thread>
#include <unordered_map>
#include <unordered_set>

#include "Logger.hpp"
#include "lib/json.hpp"

namespace Zest {

    namespace fs = std::filesystem;

    namespace {
        constexpr auto COMPACTION_CHECK_PERIOD = std::chrono::seconds(1);
        constexpr uint64_t COMPACTION_DEAD_PERCENT = 50; // matches compactSegments(), which moves segments less than half live

        // Held by the shard that is compacting: the others wait for their turn, so the shards never all block at once
        std::mutex compactionTurnMtx;
    } // namespace

    Shard::Shard(Settings& baseSettings, int shardIdNum)
        : settings(baseSettings),
          shardId(shardIdNum) {
        this->boot();
    }

    Shard::~Shard() {
        this->stop();
    }

    void Shard::boot() {
        fs::path shardPath = this->settings.DbPath / "shards" / std::to_string(shardId);
        fs::path indexPath = shardPath / "INDEX";
        fs::path walPath = shardPath / "WAL";

        if (!fs::exists(indexPath)) {
            ZestLog(LogLevel::Info, std::format("Creating INDEX for shard {} at {}", shardId, indexPath.string()));

            if (auto parent = indexPath.parent_path(); !fs::exists(parent)) {
                fs::create_directories(parent);
            }

            std::ofstream idx(indexPath);
            if (!idx) {
                ZestLog(LogLevel::Error, std::format("Failed to create INDEX for shard {}", shardId));
                throw std::runtime_error("Failed to create INDEX for shard");
            }
        }

        if (!fs::exists(shardPath / "seg")) {
            fs::create_directory(shardPath / "seg");
        }

        if (!fs::exists(walPath)) {
            ZestLog(LogLevel::Info, std::format("Creating the WAL at {}", walPath.string()));

            if (auto parent = walPath.parent_path(); !fs::exists(parent)) {
                fs::create_directories(parent);
            }

            std::ofstream walFile(walPath);
            if (!walFile) {
                ZestLog(LogLevel::Error, std::format("Failed to create WAL at {}", walPath.string()));
                throw std::runtime_error("Failed to create WAL");
            }
        }

        this->wal = std::make_unique<WAL>(this->settings, walPath);

        // From here on, the shard's components see the shard folder as their DbPath (segments go to <shard>/seg)
        this->settings.DbPath = shardPath;
        this->settings.IndexPath = indexPath;
        this->settings.WalPath = walPath;

        this->indexManager = std::make_unique<IndexManager>(this->settings);
        this->storageManager = std::make_unique<StorageManager>(this->settings);

        this->cache = std::make_unique<LRUCache>(this->settings.CacheSize);

        this->verifyIndexEntries();

        ZestLog(LogLevel::Info, std::format("Shard {} initialized successfully", this->shardId));

        // The cache is warmed on a separate thread while the compactor starts; the constructor still waits for it
        std::promise<void> cachePromise;
        std::future<void> cacheFuture = cachePromise.get_future();

        std::thread cacheThread([this, promise = std::move(cachePromise)]() mutable {
            this->fillCache();
            promise.set_value();
        });

        this->compactorThread = std::jthread([this](const std::stop_token& stopToken) {
            std::unique_lock<std::mutex> lock(this->compactorThreadMtx);

            ZestLog(LogLevel::Info, "Starting the compactor...");

            auto lastCompaction = std::chrono::steady_clock::now();
            uint64_t deadAfterLast = 0;
            while (!stopToken.stop_requested()) {
                this->threadCV.wait_for(lock, stopToken, COMPACTION_CHECK_PERIOD, [stopToken] { return stopToken.stop_requested(); });
                if (stopToken.stop_requested()) {
                    break;
                }

                unsigned int interval;
                unsigned long segSize;
                {
                    std::shared_lock<std::shared_mutex> settingsLock(this->readMtx);
                    interval = this->settings.CompactingInterval;
                    segSize = this->settings.SegSize;
                }
                const uint64_t dead = this->deadBytes();
                // At least a segment of new dead data since the last run: dead data compaction could not move (in the
                // segment being written) must not start it again at every check
                const bool fragmented = dead * 100 >= this->storageManager->totalBytes() * COMPACTION_DEAD_PERCENT && dead >= deadAfterLast + segSize;
                // Expired keys only count as dead once a compaction drops them: the interval bounds how long they stay
                const bool overdue = std::chrono::steady_clock::now() - lastCompaction >= std::chrono::seconds(interval);
                if (!fragmented && !overdue) {
                    continue;
                }

                std::unique_lock<std::mutex> turn(compactionTurnMtx, std::try_to_lock);
                if (!turn.owns_lock()) {
                    continue; // another shard is compacting: try again at the next check
                }
                this->compact();
                turn.unlock();
                lastCompaction = std::chrono::steady_clock::now();
                deadAfterLast = this->deadBytes();
                ZestLog(LogLevel::Debug, std::format("Compactor - shard {} compacted, {} dead bytes left", this->shardId, deadAfterLast));
            }
            ZestLog(LogLevel::Info, "Stopping the compactor...");
        });

        cacheFuture.wait();

        if (cacheThread.joinable()) {
            cacheThread.join();
        }
    }

    uint64_t Shard::deadBytes() {
        const uint64_t total = this->storageManager->totalBytes();
        const uint64_t live = this->indexManager->liveBytes();
        return total > live ? total - live : 0;
    }

    void Shard::compact() {
        std::unique_lock<std::shared_mutex> compactionLock(this->compactionMtx);
        std::unique_lock<std::shared_mutex> shardLock(this->readMtx);
        this->compactSegments(this->indexManager->compact());
    }

    void Shard::compactSegments(std::vector<IndexEntry> entries) {
        std::unordered_map<uint32_t, uint64_t> liveBytes;
        for (const auto& entry : entries) {
            liveBytes[entry.segmentId] += entry.size;
        }

        // A segment that is less than half live data is emptied: its live values are appended again to the current
        // segment, and the old one is deleted below once nothing points to it. The current segment is never moved,
        // since it is still being written to.
        const uint32_t current = this->storageManager->currentSegmentId();
        bool moved = false;
        for (auto& entry : entries) {
            if (entry.segmentId == current || liveBytes[entry.segmentId] * 2 >= this->settings.SegSize) {
                continue;
            }
            std::string value = this->storageManager->read(entry);
            if (value.empty()) {
                continue;
            }
            IndexEntry relocated = this->storageManager->append(value);
            if (relocated.segmentId == INVALID_SEGMENT_ID) {
                continue;
            }
            relocated.key = entry.key;
            this->indexManager->insert(relocated);
            entry = relocated;
            moved = true;
        }

        // The relocated copies and their index records must be on disk before the old segments are deleted,
        // otherwise a crash would leave index entries pointing to a removed file
        if (moved && !(this->storageManager->flush() && this->indexManager->flush())) {
            ZestLog(LogLevel::Error, std::format("Shard {} - flush failed after relocating values, keeping the old segments", this->shardId));
            return;
        }

        std::vector<uint32_t> usedSegmentIds;
        usedSegmentIds.reserve(entries.size());
        for (const auto& entry : entries) {
            usedSegmentIds.push_back(entry.segmentId);
        }
        this->storageManager->removeUnusedSegments(usedSegmentIds);
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
            if (value.empty()) {
                continue;
            }
            this->cache->put(entries[i], value);
            numKeysInserted++;
        }
        ZestLog(LogLevel::Info, std::format("Shard {} cache filled with {} keys", shardId, numKeysInserted));
    }

    ResultType Shard::get(const std::string& key) {
        auto start = std::chrono::high_resolution_clock::now();
        std::shared_lock<std::shared_mutex> lock(this->readMtx);

        CacheEntry cacheEntry = this->cache->get(key);

        bool fromCache = (cacheEntry.index.segmentId != INVALID_SEGMENT_ID && !cacheEntry.index.isTombstone && !cacheEntry.index.isExpired());

        if (fromCache) {
            auto end = std::chrono::high_resolution_clock::now();
            double latency = std::chrono::duration<double, std::milli>(end - start).count();
            this->perfMonitor.addGetStats(false, latency);
            return { ResultType::Code::Success, cacheEntry.value, 1 };
        }

        IndexEntry entry;
        entry = this->indexManager->search(key);

        if (entry.segmentId != INVALID_SEGMENT_ID && !entry.isTombstone) {
            std::string value = this->storageManager->read(entry);
            if (value.empty()) {
                return { ResultType::Code::ReadFailed };
            }

            this->cache->put(entry, value);

            auto end = std::chrono::high_resolution_clock::now();
            double latency = std::chrono::duration<double, std::milli>(end - start).count();
            this->perfMonitor.addGetStats(true, latency);
            return { ResultType::Code::Success, value, 1 };
        }

        auto end = std::chrono::high_resolution_clock::now();
        double latency = std::chrono::duration<double, std::milli>(end - start).count();
        this->perfMonitor.addGetStats(true, latency);
        return { ResultType::Code::KeyNotFound };
    }

    ResultType Shard::set(const std::string& key, const std::string& value, uint64_t expiresAt) {
        std::unique_lock<std::shared_mutex> lock(this->readMtx);
        return this->writeLocked(key, value, expiresAt);
    }

    ResultType Shard::setnx(const std::string& key, const std::string& value) {
        std::unique_lock<std::shared_mutex> lock(this->readMtx);
        IndexEntry existing = this->indexManager->search(key);
        if (existing.segmentId != INVALID_SEGMENT_ID && !existing.isTombstone) {
            return { ResultType::Code::KeyExists };
        }
        return this->writeLocked(key, value, 0);
    }

    ResultType Shard::replace(const std::string& key, const std::string& value, uint64_t& expiresAt) {
        std::unique_lock<std::shared_mutex> lock(this->readMtx);
        IndexEntry existing = this->indexManager->search(key);
        if (existing.segmentId == INVALID_SEGMENT_ID || existing.isTombstone) {
            expiresAt = 0;
            return { ResultType::Code::KeyNotFound };
        }
        expiresAt = existing.expiresAt;
        return this->writeLocked(key, value, expiresAt);
    }

    ResultType Shard::writeLocked(const std::string& key, const std::string& value, uint64_t expiresAt) {
        auto start = std::chrono::high_resolution_clock::now();

        // The value goes to disk first, then the index points to it: a crash in between leaves an orphan value,
        // never an index entry pointing to nothing
        IndexEntry entry = this->storageManager->append(value);

        if (entry.segmentId == INVALID_SEGMENT_ID) {
            ZestLog(LogLevel::Error, std::format("Shard::set - FAILED to write key: {} in shard {} "
                                                 "- segment id {}",
                                                 key, shardId, entry.segmentId));
            auto end = std::chrono::high_resolution_clock::now();
            double latency = std::chrono::duration<double, std::milli>(end - start).count();
            this->perfMonitor.addSetStats(false, latency);
            return { ResultType::Code::WriteFailed };
        }

        entry.key = key;
        entry.expiresAt = expiresAt;

        this->indexManager->insert(entry);

        // Only refresh a key that is already cached: writes must not evict the keys that are read often
        this->cache->update(entry, value);

        auto end = std::chrono::high_resolution_clock::now();
        double latency = std::chrono::duration<double, std::milli>(end - start).count();
        this->perfMonitor.addSetStats(false, latency);
        return { ResultType::Code::Success, "", 1 };
    }

    ResultType Shard::del(const std::string& key) {
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
            return { ResultType::Code::Success, "", 1 };
        }

        auto end = std::chrono::high_resolution_clock::now();
        double latency = std::chrono::duration<double, std::milli>(end - start).count();
        this->perfMonitor.addDelStats(false, latency);
        return { ResultType::Code::KeyNotFound };
    }

    ResultType Shard::setTTL(const std::string& key, uint64_t expiresAt) {
        std::unique_lock<std::shared_mutex> lock(this->readMtx);

        IndexEntry entry = this->indexManager->search(key);
        if (entry.segmentId == INVALID_SEGMENT_ID || entry.isTombstone) {
            return { ResultType::Code::KeyNotFound };
        }

        entry.expiresAt = expiresAt;
        this->indexManager->update(key, entry);
        this->cache->remove(key);
        return { ResultType::Code::Success, "", 1 };
    }

    ResultType Shard::getTTL(const std::string& key) {
        std::shared_lock<std::shared_mutex> lock(this->readMtx);

        IndexEntry entry = this->indexManager->search(key);
        if (entry.segmentId == INVALID_SEGMENT_ID || entry.isTombstone) {
            return { ResultType::Code::Success, "-2", 0 };
        }
        if (entry.expiresAt == 0) {
            return { ResultType::Code::Success, "-1", 1 };
        }
        const uint64_t now = nowMs();
        if (entry.expiresAt <= now) {
            return { ResultType::Code::Success, "-2", 0 };
        }
        // Rounded up, so a key that still has a few milliseconds left reports 1 second rather than 0
        return { ResultType::Code::Success, std::to_string((entry.expiresAt - now + 999) / 1000), 1 };
    }

    ResultType Shard::getBy(ValidationRule& valid) {
        auto start = std::chrono::high_resolution_clock::now();
        std::shared_lock<std::shared_mutex> lock(this->readMtx);
        std::vector<IndexEntry> entries;
        // The shards run in parallel and share globalMatchCount: stop scanning once the others already took the whole limit
        entries = this->indexManager->findMatching(valid.func, [&valid]() { return valid.limit != UINT_MAX && valid.globalMatchCount && valid.globalMatchCount->load() >= valid.limit; });

        nlohmann::json resultArray = nlohmann::json::array();
        int matchCount = 0;

        for (const IndexEntry& entry : entries) {
            if (entry.isTombstone || entry.segmentId == INVALID_SEGMENT_ID) {
                continue;
            }
            // Reserve one slot of the global limit; if another shard took the last one, give it back and stop
            if (valid.limit != UINT_MAX && valid.globalMatchCount) {
                unsigned int prev = valid.globalMatchCount->fetch_add(1);
                if (prev >= valid.limit) {
                    valid.globalMatchCount->fetch_sub(1);
                    break;
                }
            }
            std::string value = this->storageManager->read(entry);
            if (value.empty()) {
                continue;
            }
            resultArray.push_back(nlohmann::json::object({ { entry.key, value } }));
            matchCount++;
        }

        auto end = std::chrono::high_resolution_clock::now();
        double latency = std::chrono::duration<double, std::milli>(end - start).count();
        this->perfMonitor.addGetByStats(false, latency);

        return { ResultType::Code::Success, resultArray.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace), matchCount };
    }

    ResultType Shard::setBy(ValidationRule& valid, const std::string& value) {
        auto start = std::chrono::high_resolution_clock::now();
        std::unique_lock<std::shared_mutex> lock(this->readMtx);
        std::vector<IndexEntry> entries;
        entries = this->indexManager->findMatching(valid.func, [&valid]() { return valid.limit != UINT_MAX && valid.globalMatchCount && valid.globalMatchCount->load() >= valid.limit; });

        int matchCount = 0;

        for (const IndexEntry& entry : entries) {
            if (entry.isTombstone || entry.segmentId == INVALID_SEGMENT_ID) {
                continue;
            }
            if (valid.limit != UINT_MAX && valid.globalMatchCount) {
                unsigned int prev = valid.globalMatchCount->fetch_add(1);
                if (prev >= valid.limit) {
                    valid.globalMatchCount->fetch_sub(1);
                    break;
                }
            }
            IndexEntry newEntry = this->storageManager->append(value);

            if (newEntry.segmentId == INVALID_SEGMENT_ID) {
                if (valid.limit != UINT_MAX && valid.globalMatchCount) {
                    valid.globalMatchCount->fetch_sub(1);
                }
                ZestLog(LogLevel::Error, std::format("Shard::setBy - FAILED to write for key: "
                                                     "{} - segment full",
                                                     entry.key));
                continue;
            }

            newEntry.key = entry.key;
            newEntry.expiresAt = entry.expiresAt; // setby changes the value, not the expiry

            this->indexManager->insert(newEntry);

            this->cache->update(newEntry, value);

            matchCount++;
        }

        auto end = std::chrono::high_resolution_clock::now();
        double latency = std::chrono::duration<double, std::milli>(end - start).count();
        this->perfMonitor.addSetByStats(false, latency);

        return { ResultType::Code::Success, "", matchCount };
    }

    ResultType Shard::delBy(ValidationRule& valid) {
        auto start = std::chrono::high_resolution_clock::now();
        std::unique_lock<std::shared_mutex> lock(this->readMtx);
        std::vector<IndexEntry> entries;
        entries = this->indexManager->findMatching(valid.func, [&valid]() { return valid.limit != UINT_MAX && valid.globalMatchCount && valid.globalMatchCount->load() >= valid.limit; });

        int matchCount = 0;

        for (const IndexEntry& entry : entries) {
            if (entry.isTombstone || entry.segmentId == INVALID_SEGMENT_ID) {
                continue;
            }
            if (valid.limit != UINT_MAX && valid.globalMatchCount) {
                unsigned int prev = valid.globalMatchCount->fetch_add(1);
                if (prev >= valid.limit) {
                    valid.globalMatchCount->fetch_sub(1);
                    break;
                }
            }
            IndexEntry tombstoneEntry = entry;
            tombstoneEntry.isTombstone = true;

            this->indexManager->update(entry.key, tombstoneEntry);
            this->cache->remove(entry.key);

            matchCount++;
        }

        auto end = std::chrono::high_resolution_clock::now();
        double latency = std::chrono::duration<double, std::milli>(end - start).count();
        this->perfMonitor.addDelByStats(false, latency);

        return { ResultType::Code::Success, "", matchCount };
    }

    ResultType Shard::readLocked(const std::string& key, uint64_t& expiresAt) {
        expiresAt = 0;
        CacheEntry cached = this->cache->get(key);
        if (cached.index.segmentId == INVALID_SEGMENT_ID || cached.index.isTombstone || cached.index.isExpired()) {
            cached.index = this->indexManager->search(key);
            if (cached.index.segmentId == INVALID_SEGMENT_ID || cached.index.isTombstone) {
                return { ResultType::Code::KeyNotFound };
            }
            cached.value = this->storageManager->read(cached.index);
            if (cached.value.empty()) {
                return { ResultType::Code::ReadFailed };
            }
        }
        expiresAt = cached.index.expiresAt;
        return { ResultType::Code::Success, cached.value, 1 };
    }

    ResultType Shard::incr(const std::string& key, int64_t delta, uint64_t& expiresAt) {
        std::unique_lock<std::shared_mutex> lock(this->readMtx);

        int64_t current = 0;
        ResultType stored = this->readLocked(key, expiresAt);
        if (stored.code == ResultType::Code::Success) {
            const std::string& value = stored.response;
            auto [end, ec] = std::from_chars(value.data(), value.data() + value.size(), current);
            if (ec != std::errc() || end != value.data() + value.size()) {
                return { ResultType::Code::NotAnInteger };
            }
        } else if (stored.code != ResultType::Code::KeyNotFound) {
            return stored;
        }

        if ((delta > 0 && current > INT64_MAX - delta) || (delta < 0 && current < INT64_MIN - delta)) {
            return { ResultType::Code::IntegerOverflow };
        }
        const std::string result = std::to_string(current + delta);

        ResultType written = this->writeLocked(key, result, expiresAt);
        if (written.code == ResultType::Code::Success) {
            written.response = result;
        }
        return written;
    }

    ResultType Shard::cas(const std::string& key, const std::string& expected, const std::string& value, uint64_t& expiresAt) {
        std::unique_lock<std::shared_mutex> lock(this->readMtx);

        ResultType stored = this->readLocked(key, expiresAt);
        if (stored.code != ResultType::Code::Success) {
            return stored;
        }
        if (stored.response != expected) {
            return { ResultType::Code::ValueMismatch };
        }
        return this->writeLocked(key, value, expiresAt);
    }

    ResultType Shard::exists(const std::string& key) {
        auto start = std::chrono::high_resolution_clock::now();
        std::shared_lock<std::shared_mutex> lock(this->readMtx);
        IndexEntry entry = this->indexManager->search(key);
        const bool found = entry.segmentId != INVALID_SEGMENT_ID && !entry.isTombstone;
        auto end = std::chrono::high_resolution_clock::now();
        this->perfMonitor.addExistsStats(false, std::chrono::duration<double, std::milli>(end - start).count());
        return { ResultType::Code::Success, found ? "true" : "false", found ? 1 : 0 };
    }

    size_t Shard::count(const ValidationRule& valid) {
        auto start = std::chrono::high_resolution_clock::now();
        std::shared_lock<std::shared_mutex> lock(this->readMtx);
        const size_t n = this->indexManager->count(valid.func);
        auto end = std::chrono::high_resolution_clock::now();
        this->perfMonitor.addCountStats(false, std::chrono::duration<double, std::milli>(end - start).count());
        return n;
    }

    std::vector<std::string> Shard::matchingKeys(const ValidationRule& valid, bool recordKeysStats) {
        auto start = std::chrono::high_resolution_clock::now();
        std::shared_lock<std::shared_mutex> lock(this->readMtx);
        std::vector<std::string> keys = this->indexManager->keys(valid.func);
        // Keep the `limit` smallest keys in O(n) without sorting them all; ShardManager merges the shards and sorts the result
        if (keys.size() > valid.limit) {
            std::nth_element(keys.begin(), keys.begin() + valid.limit, keys.end());
            keys.resize(valid.limit);
        }
        if (recordKeysStats) {
            auto end = std::chrono::high_resolution_clock::now();
            this->perfMonitor.addKeysStats(false, std::chrono::duration<double, std::milli>(end - start).count());
        }
        return keys;
    }

    bool Shard::flushData() {
        std::unique_lock<std::shared_mutex> lock(this->readMtx);
        if (this->storageManager->flush() && this->indexManager->flush()) {
            return true;
        }
        ZestLog(LogLevel::Error, std::format("Shard {} - flush failed", this->shardId));
        return false;
    }

    void Shard::flush() {
        // The WAL is only cleared once the data it protects is synced: a failed flush keeps it for the next replay
        if (this->flushData()) {
            this->wal->clear();
        }
    }

    void Shard::stop() {
        if (!this->isStopped.load()) {
            ZestLog(LogLevel::Info, std::format("Exiting shard {}...", shardId));

            this->compactorThread.request_stop();
            this->threadCV.notify_all();

            this->flushData();

            this->isStopped.store(true);
        }
    }

    void Shard::verifyIndexEntries() {
        ZestLog(LogLevel::Info, std::format("Shard {} - Verifying index entries...", this->shardId));

        std::vector<IndexEntry> entries = this->indexManager->getAll();
        int verifiedCount = 0;
        int removedCount = 0;

        for (const auto& entry : entries) {
            if (entry.isTombstone || entry.segmentId == INVALID_SEGMENT_ID) {
                continue;
            }

            if (!this->storageManager->contains(entry)) {
                ZestLog(LogLevel::Warning, std::format("Shard {} - Removing invalid index entry for key: {}", this->shardId, entry.key));

                IndexEntry tombstoneEntry = entry;
                tombstoneEntry.isTombstone = true;
                this->indexManager->update(entry.key, tombstoneEntry);
                removedCount++;
            } else {
                verifiedCount++;
            }
        }

        ZestLog(LogLevel::Info, std::format("Shard {} - Index verification complete: {} valid, {} removed", this->shardId, verifiedCount, removedCount));
    }

    void Shard::reloadSettings(Settings& set) {
        fs::path shardPath = set.DbPath / "shards" / std::to_string(shardId);
        fs::path indexPath = shardPath / "INDEX";
        fs::path walPath = shardPath / "WAL";

        std::unique_lock<std::shared_mutex> lock(this->readMtx);
        this->settings = set;

        this->settings.DbPath = shardPath;
        this->settings.IndexPath = indexPath;
        this->settings.WalPath = walPath;

        this->cache->resize(set.CacheSize);
    }

} // namespace Zest
