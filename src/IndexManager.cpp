#include "IndexManager.hpp"

#include <chrono>
#include <format>

#include "File.hpp"
#include "Logger.hpp"

namespace Zest {

    IndexManager::IndexManager(Settings &set)
        : settings(set),
          canFlush(false) {
        ZestLog(LogLevel::INFO, "Opening INDEX file...");
        this->indexPath = set.IndexPath;

        std::filesystem::path indexTmpPath(this->settings.DbPath / "INDEX.tmp");

        if (std::filesystem::exists(indexTmpPath) && std::filesystem::file_size(indexTmpPath) > 0) {
            std::filesystem::remove(this->indexPath);
            std::filesystem::copy_file(indexTmpPath, this->indexPath);
            std::filesystem::remove(indexTmpPath);
        }

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
        std::unique_lock<std::shared_mutex> lock(this->mtx);
        this->index.seekg(0, std::ios::end);
        std::streamoff fsize = this->index.tellg();

        this->index.seekg(0, std::ios::beg);

        while (this->index.tellg() < fsize) {
            std::streamoff position = this->index.tellg();
            IndexEntry entry;
            if (!entry.deserialize(this->index))
                break;

            if (entry.isTombstone) {
                this->tombstoneOffsets.push_back(position);
            } else {
                this->memoryTree[entry.key] = position;
            }
        }
        ZestLog(LogLevel::INFO, "IndexManager - Loaded entries into memory tree.");
    }

    IndexEntry IndexManager::search(const std::string &key) {
        std::shared_lock<std::shared_mutex> lock(this->mtx);

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

    void IndexManager::update(const std::string &key, const IndexEntry &entry) {
        std::unique_lock<std::shared_mutex> lock(this->mtx);

        auto it = this->memoryTree.find(key);
        if (it != this->memoryTree.end()) {
            std::streamoff offset = it->second;

            this->index.seekp(offset, std::ios::beg);
            entry.serialize(this->index);
            this->canFlush = true;
            if (entry.isTombstone) {
                this->memoryTree.erase(it);
                this->tombstoneOffsets.push_back(offset);
            }
        }
    }

    void IndexManager::insert(const IndexEntry &entry) {
        std::unique_lock<std::shared_mutex> lock(this->mtx);

        auto it = this->memoryTree.find(entry.key);
        if (it != this->memoryTree.end()) {
            std::streamoff oldOffset = it->second;
            IndexEntry oldEntry;
            this->index.seekg(oldOffset, std::ios::beg);
            if (oldEntry.deserialize(this->index)) {
                oldEntry.isTombstone = true;
                this->index.seekp(oldOffset, std::ios::beg);
                oldEntry.serialize(this->index);
                this->tombstoneOffsets.push_back(oldOffset);
            }

            this->index.seekp(0, std::ios::end);
            std::streamoff newOffset = this->index.tellp();
            entry.serialize(this->index);
            this->canFlush = true;
            this->memoryTree[entry.key] = newOffset;
            return;
        }

        std::streamoff insertPosition;

        if (!this->tombstoneOffsets.empty()) {
            insertPosition = this->tombstoneOffsets.back();
            this->tombstoneOffsets.pop_back();
        } else {
            this->index.seekp(0, std::ios::end);
            insertPosition = this->index.tellp();
        }

        this->index.seekp(insertPosition, std::ios::beg);
        entry.serialize(this->index);
        this->canFlush = true;
        this->memoryTree[entry.key] = insertPosition;
    }

    std::vector<IndexEntry> IndexManager::getAll(unsigned int limit, const std::function<bool()> &stopEarly) {
        std::vector<IndexEntry> res;
        std::shared_lock<std::shared_mutex> lock(this->mtx);

        res.reserve(this->memoryTree.size());

        for (auto const &[key, offset] : this->memoryTree) {
            if (res.size() >= limit)
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

    std::vector<IndexEntry> IndexManager::compact() {
        ZestLog(LogLevel::DEBUG, "Starting index compaction...");
        std::filesystem::copy_file(this->settings.IndexPath, this->settings.DbPath / "INDEX.tmp", std::filesystem::copy_options::overwrite_existing);

        std::vector<IndexEntry> entries = this->getAll();

        std::unique_lock<std::shared_mutex> lock(this->mtx);

        this->index.close();
        this->index.open(this->indexPath, std::ios::out | std::ios::in | std::ios::binary | std::ios::trunc);

        if (!this->index.is_open()) {
            ZestLog(LogLevel::ERROR, "Compact failed: could not reopen index file.");
            return entries;
        }

        this->memoryTree.clear();
        this->tombstoneOffsets.clear();

        std::vector<IndexEntry> result;

        for (const auto &entry : entries) {
            if (entry.isTombstone || entry.segmentId == INVALID_SEGMENT_ID) {
                continue;
            }
            std::streamoff newPos = this->index.tellp();

            entry.serialize(this->index);
            if (this->index.good()) {
                this->memoryTree[entry.key] = newPos;
                result.push_back(entry);
            } else {
                ZestLog(LogLevel::ERROR, std::format("Compact - Failed to write entry for key: {}", entry.key));
            }
        }

        this->canFlush = true;
        this->flush();
        ZestLog(LogLevel::DEBUG, "Index compaction completed successfully.");

        std::filesystem::remove(this->settings.DbPath / "INDEX.tmp");
        return result;
    }

    void IndexManager::flush() {
        std::unique_lock<std::shared_mutex> lock(this->mtx, std::try_to_lock);
        if (!lock.owns_lock()) {
            return;
        }

        if (this->canFlush) {
            flush_and_fsync(this->index);
            this->canFlush = false;
        }
    }

} // namespace Zest
