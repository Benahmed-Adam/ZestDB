#include "IndexManager.hpp"

#include <chrono>
#include <format>

#include "File.hpp"
#include "Logger.hpp"

namespace Zest {

    IndexManager::IndexManager(Settings& set)
        : settings(set),
          canFlush(false) {
        ZestLog(LogLevel::Info, "Opening INDEX file...");
        this->indexPath = set.IndexPath;

        std::filesystem::remove(this->settings.DbPath / "INDEX.tmp");

        this->index.open(this->indexPath, std::ios::in | std::ios::out | std::ios::binary);
        if (!this->index.is_open()) {
            this->index.open(this->indexPath, std::ios::out | std::ios::binary);
            this->index.close();
            this->index.open(this->indexPath, std::ios::in | std::ios::out | std::ios::binary);
        }

        this->loadIndexIntoMemory();
        this->compact();
    }

    IndexManager::~IndexManager() {
        if (this->index.is_open()) {
            this->index.close();
        }
    }

    IndexEntry IndexManager::toEntry(const std::string& key, const Location& loc) {
        return { key, loc.offset, loc.segmentId, loc.size, false, loc.expiresAt };
    }

    void IndexManager::loadIndexIntoMemory() {
        std::unique_lock<std::mutex> lock(this->mtx);
        this->index.seekg(0, std::ios::end);
        std::streamoff fsize = this->index.tellg();

        this->index.seekg(0, std::ios::beg);

        std::streamoff position = 0;
        while (position < fsize) {
            IndexEntry entry;
            if (!entry.deserialize(this->index))
                break;

            if (entry.isTombstone) {
                this->memoryTree.erase(entry.key);
            } else {
                this->memoryTree[entry.key] = { entry.offset, entry.segmentId, entry.size, entry.expiresAt };
            }
            position = this->index.tellg();
        }

        this->index.clear();
        if (position < fsize) {
            ZestLog(LogLevel::Warning, std::format("IndexManager - Truncating {} unreadable bytes at the end of the INDEX", fsize - position));
            this->index.close();
            std::filesystem::resize_file(this->indexPath, static_cast<std::uintmax_t>(position));
            this->index.open(this->indexPath, std::ios::in | std::ios::out | std::ios::binary);
        }
        this->index.seekp(0, std::ios::end);
        this->recomputeLiveSize();
        ZestLog(LogLevel::Info, "IndexManager - Loaded entries into memory tree.");
    }

    void IndexManager::recomputeLiveSize() {
        uint64_t total = 0;
        for (const auto& [key, loc] : this->memoryTree) {
            total += loc.size;
        }
        this->liveSize = total;
    }

    void IndexManager::append(const IndexEntry& entry) {
        this->index.clear();
        entry.serialize(this->index);
        this->canFlush = true;
    }

    IndexEntry IndexManager::search(const std::string& key) {
        std::unique_lock<std::mutex> lock(this->mtx);

        auto it = this->memoryTree.find(key);
        if (it != this->memoryTree.end() && !toEntry(it->first, it->second).isExpired()) {
            return toEntry(it->first, it->second);
        }

        return { "", INVALID_OFFSET, INVALID_SEGMENT_ID, 0, false };
    }

    void IndexManager::update(const std::string& key, const IndexEntry& entry) {
        std::unique_lock<std::mutex> lock(this->mtx);

        auto it = this->memoryTree.find(key);
        if (it == this->memoryTree.end()) {
            return;
        }

        IndexEntry record = entry;
        record.key = key;
        this->append(record);
        this->liveSize -= it->second.size;
        if (entry.isTombstone) {
            this->memoryTree.erase(it);
        } else {
            it->second = { entry.offset, entry.segmentId, entry.size, entry.expiresAt };
            this->liveSize += entry.size;
        }
    }

    void IndexManager::insert(const IndexEntry& entry) {
        std::unique_lock<std::mutex> lock(this->mtx);
        this->append(entry);
        const Location loc = { entry.offset, entry.segmentId, entry.size, entry.expiresAt };
        auto [it, inserted] = this->memoryTree.try_emplace(entry.key, loc);
        if (!inserted) {
            this->liveSize -= it->second.size;
            it->second = loc;
        }
        this->liveSize += entry.size;
    }

    std::vector<IndexEntry> IndexManager::getAll(unsigned int limit, const std::function<bool()>& stopEarly) {
        std::vector<IndexEntry> res;
        std::unique_lock<std::mutex> lock(this->mtx);

        res.reserve(std::min<size_t>(limit, this->memoryTree.size()));
        const uint64_t now = nowMs();
        for (const auto& [key, loc] : this->memoryTree) {
            if (res.size() >= limit)
                break;
            if (stopEarly && stopEarly())
                break;
            IndexEntry entry = toEntry(key, loc);
            if (!entry.isExpired(now)) {
                res.push_back(std::move(entry));
            }
        }

        return res;
    }

    std::vector<IndexEntry> IndexManager::findMatching(const std::function<bool(std::string_view)>& matches, const std::function<bool()>& stopEarly) {
        std::vector<IndexEntry> res;
        std::unique_lock<std::mutex> lock(this->mtx);

        const uint64_t now = nowMs();
        for (const auto& [key, loc] : this->memoryTree) {
            if (stopEarly && stopEarly())
                break;
            if (matches(key) && !toEntry(key, loc).isExpired(now)) {
                res.push_back(toEntry(key, loc));
            }
        }

        return res;
    }

    size_t IndexManager::count(const std::function<bool(std::string_view)>& matches) {
        std::unique_lock<std::mutex> lock(this->mtx);

        const uint64_t now = nowMs();
        size_t n = 0;
        for (const auto& [key, loc] : this->memoryTree) {
            const bool expired = loc.expiresAt != 0 && loc.expiresAt <= now;
            if (!expired && (!matches || matches(key))) {
                n++;
            }
        }
        return n;
    }

    std::vector<std::string> IndexManager::keys(const std::function<bool(std::string_view)>& matches) {
        std::unique_lock<std::mutex> lock(this->mtx);
        std::vector<std::string> res;
        const uint64_t now = nowMs();
        for (const auto& [key, loc] : this->memoryTree) {
            const bool expired = loc.expiresAt != 0 && loc.expiresAt <= now;
            if (!expired && (!matches || matches(key))) {
                res.push_back(key);
            }
        }
        return res;
    }

    std::vector<IndexEntry> IndexManager::compact() {
        ZestLog(LogLevel::Debug, "Starting index compaction...");

        std::vector<IndexEntry> entries = this->getAll();

        std::unique_lock<std::mutex> lock(this->mtx);

        const std::filesystem::path tmpPath = this->settings.DbPath / "INDEX.tmp";
        std::fstream tmp(tmpPath, std::ios::out | std::ios::in | std::ios::binary | std::ios::trunc);
        for (const auto& entry : entries) {
            entry.serialize(tmp);
        }

        std::error_code ec;
        if (!tmp.good() || !flush_and_fsync(tmp, tmpPath)) {
            ZestLog(LogLevel::Error, "Compact failed: could not write INDEX.tmp, keeping the current INDEX");
            tmp.close();
            std::filesystem::remove(tmpPath, ec);
            return entries;
        }
        tmp.close();

        this->index.close();
        std::filesystem::rename(tmpPath, this->indexPath, ec);
        this->index.open(this->indexPath, std::ios::in | std::ios::out | std::ios::binary);
        this->index.seekp(0, std::ios::end);
        if (ec || !this->index.is_open()) {
            ZestLog(LogLevel::Error, std::format("Compact failed: could not replace the INDEX: {}", ec.message()));
            return entries;
        }

        this->canFlush = false;
        std::erase_if(this->memoryTree, [now = nowMs()](const auto& item) { return toEntry(item.first, item.second).isExpired(now); });
        this->recomputeLiveSize();
        ZestLog(LogLevel::Debug, "Index compaction completed successfully.");
        return entries;
    }

    bool IndexManager::flush() {
        std::unique_lock<std::mutex> lock(this->mtx);

        if (this->canFlush) {
            if (!flush_and_fsync(this->index, this->indexPath)) {
                return false;
            }
            this->canFlush = false;
        }
        return true;
    }

} // namespace Zest
