#include "IndexManager.hpp"

#include <chrono>
#include <format>

#include "File.hpp"
#include "Logger.hpp"

namespace Zest {

    IndexManager::IndexManager(Settings& set)
        : settings(set),
          canFlush(false) {
        ZestLog(LogLevel::INFO, "Opening INDEX file...");
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

            if (!entry.isTombstone) {
                this->memoryTree[entry.key] = position;
            }
            position = this->index.tellg();
        }

        this->index.clear();
        if (position < fsize) {
            ZestLog(LogLevel::WARNING, std::format("IndexManager - Truncating {} unreadable bytes at the end of the INDEX", fsize - position));
            this->index.close();
            std::filesystem::resize_file(this->indexPath, static_cast<std::uintmax_t>(position));
            this->index.open(this->indexPath, std::ios::in | std::ios::out | std::ios::binary);
        }
        ZestLog(LogLevel::INFO, "IndexManager - Loaded entries into memory tree.");
    }

    IndexEntry IndexManager::search(const std::string& key) {
        std::unique_lock<std::mutex> lock(this->mtx);
        this->index.clear();

        auto it = this->memoryTree.find(key);
        if (it != this->memoryTree.end()) {
            std::streamoff offset = it->second;

            this->index.seekg(offset, std::ios::beg);
            IndexEntry entry;

            if (entry.deserialize(this->index) && !entry.isTombstone) {
                return entry;
            }
        }

        return { "", INVALID_OFFSET, INVALID_SEGMENT_ID, 0, false };
    }

    void IndexManager::update(const std::string& key, const IndexEntry& entry) {
        std::unique_lock<std::mutex> lock(this->mtx);
        this->index.clear();

        auto it = this->memoryTree.find(key);
        if (it != this->memoryTree.end()) {
            std::streamoff offset = it->second;

            this->index.seekp(offset, std::ios::beg);
            entry.serialize(this->index);
            this->canFlush = true;
            if (entry.isTombstone) {
                this->memoryTree.erase(it);
            }
        }
    }

    void IndexManager::insert(const IndexEntry& entry) {
        std::unique_lock<std::mutex> lock(this->mtx);
        this->index.clear();

        auto it = this->memoryTree.find(entry.key);
        if (it != this->memoryTree.end()) {
            std::streamoff oldOffset = it->second;
            IndexEntry oldEntry;
            this->index.seekg(oldOffset, std::ios::beg);
            if (oldEntry.deserialize(this->index)) {
                oldEntry.isTombstone = true;
                this->index.seekp(oldOffset, std::ios::beg);
                oldEntry.serialize(this->index);
            }
        }

        this->index.seekp(0, std::ios::end);
        std::streamoff insertPosition = this->index.tellp();
        entry.serialize(this->index);
        this->canFlush = true;
        this->memoryTree[entry.key] = insertPosition;
    }

    std::vector<IndexEntry> IndexManager::getAll(unsigned int limit, const std::function<bool()>& stopEarly) {
        std::vector<IndexEntry> res;
        std::unique_lock<std::mutex> lock(this->mtx);
        this->index.clear();

        res.reserve(this->memoryTree.size());

        for (auto const& [key, offset] : this->memoryTree) {
            if (res.size() >= limit || limit == 0)
                break;
            if (stopEarly && stopEarly())
                break;

            this->index.seekg(offset, std::ios::beg);

            IndexEntry e;
            if (e.deserialize(this->index)) {
                res.push_back(std::move(e));
            } else {
                ZestLog(LogLevel::ERROR, std::format("IndexManager::getAll - Failed to read "
                                                     "entry at offset: {}",
                                                     offset));
                this->index.clear();
            }
        }

        return res;
    }

    std::vector<std::string> IndexManager::keys() {
        std::unique_lock<std::mutex> lock(this->mtx);
        std::vector<std::string> res;
        res.reserve(this->memoryTree.size());
        for (const auto& [key, offset] : this->memoryTree) {
            res.push_back(key);
        }
        return res;
    }

    std::vector<IndexEntry> IndexManager::compact() {
        ZestLog(LogLevel::DEBUG, "Starting index compaction...");

        std::vector<IndexEntry> entries = this->getAll();
        std::erase_if(entries, [](const IndexEntry& e) { return e.isTombstone || e.segmentId == INVALID_SEGMENT_ID; });

        std::unique_lock<std::mutex> lock(this->mtx);

        const std::filesystem::path tmpPath = this->settings.DbPath / "INDEX.tmp";
        std::fstream tmp(tmpPath, std::ios::out | std::ios::in | std::ios::binary | std::ios::trunc);
        std::unordered_map<std::string, std::streamoff> newTree;
        for (const auto& entry : entries) {
            newTree[entry.key] = tmp.tellp();
            entry.serialize(tmp);
        }

        std::error_code ec;
        if (!tmp.good() || !flush_and_fsync(tmp)) {
            ZestLog(LogLevel::ERROR, "Compact failed: could not write INDEX.tmp, keeping the current INDEX");
            tmp.close();
            std::filesystem::remove(tmpPath, ec);
            return entries;
        }
        tmp.close();

        this->index.close();
        std::filesystem::rename(tmpPath, this->indexPath, ec);
        this->index.open(this->indexPath, std::ios::in | std::ios::out | std::ios::binary);
        if (ec || !this->index.is_open()) {
            ZestLog(LogLevel::ERROR, std::format("Compact failed: could not replace the INDEX: {}", ec.message()));
            return entries;
        }

        this->memoryTree = std::move(newTree);
        this->canFlush = false;
        ZestLog(LogLevel::DEBUG, "Index compaction completed successfully.");
        return entries;
    }

    bool IndexManager::flush() {
        std::unique_lock<std::mutex> lock(this->mtx);

        if (this->canFlush) {
            if (!flush_and_fsync(this->index)) {
                return false;
            }
            this->canFlush = false;
        }
        return true;
    }

} // namespace Zest
