#include "IndexManager.hpp"
#include "Logger.hpp"
#include <chrono>
#include <format>

IndexManager::IndexManager(const Settings& set)
    : settings(set)
    , canFlush(false)
{
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

IndexManager::~IndexManager()
{
    if (this->index.is_open()) {
        this->index.close();
    }
}

void IndexManager::loadIndexIntoMemory()
{
    std::unique_lock<std::shared_mutex> lock(this->mtx);
    this->index.seekg(0, std::ios::end);
    std::streamoff fsize = this->index.tellg();

    std::streamoff position = 0;
    IndexEntry entry;

    while (position < fsize) {
        this->index.seekg(position, std::ios::beg);
        if (!this->index.read((char*)&entry, sizeof(entry)))
            break;

        std::string entryKey(entry.key);
        if (entry.isTombstone) {
            this->tombstoneOffsets.push_back(position);
        } else {
            this->memoryTree[entryKey] = position;
        }
        position += static_cast<std::streamoff>(sizeof(IndexEntry));
    }
    ZestLog(LogLevel::INFO, "IndexManager - Loaded entries into memory tree.");
}

IndexEntry IndexManager::search(const std::string& key)
{
    ZestLog(LogLevel::DEBUG, std::format("IndexManager::search - searching for key: {}", key));
    std::shared_lock<std::shared_mutex> lock(this->mtx);

    auto it = this->memoryTree.find(key);
    if (it != this->memoryTree.end()) {
        std::streamoff offset = it->second;

        this->index.seekg(offset, std::ios::beg);
        IndexEntry entry;

        if (this->index.read((char*)&entry, sizeof(entry)) && !entry.isTombstone) {
            ZestLog(LogLevel::DEBUG, std::format("IndexManager::search - found key: {}", key));
            return entry;
        }
    }

    ZestLog(LogLevel::DEBUG, std::format("IndexManager::search - key not found: {}", key));
    return { "", -1, 0, 0, false };
}

void IndexManager::update(const std::string& key, const IndexEntry& entry)
{
    ZestLog(LogLevel::DEBUG, std::format("IndexManager::update - updating key: {}", key));
    std::unique_lock<std::shared_mutex> lock(this->mtx);

    auto it = this->memoryTree.find(key);
    if (it != this->memoryTree.end()) {
        std::streamoff offset = it->second;

        this->index.seekp(offset, std::ios::beg);
        this->index.write((const char*)&entry, sizeof(entry));
        this->canFlush = true;
        if (entry.isTombstone) {
            this->memoryTree.erase(it);
            this->tombstoneOffsets.push_back(offset);
        }

        ZestLog(LogLevel::DEBUG, std::format("IndexManager::update - key updated at offset: {}", offset));
    } else {
        ZestLog(LogLevel::DEBUG, std::format("IndexManager::update - key not found for update: {}", key));
    }
}

void IndexManager::insert(const IndexEntry& entry)
{
    std::string keyStr(entry.key);
    ZestLog(LogLevel::DEBUG, std::format("IndexManager::insert - inserting key: {}", keyStr));
    std::unique_lock<std::shared_mutex> lock(this->mtx);

    auto it = this->memoryTree.find(keyStr);
    if (it != this->memoryTree.end()) {
        ZestLog(LogLevel::DEBUG, "IndexManager::insert - key exists, marking old entry as tombstone");

        std::streamoff oldOffset = it->second;
        IndexEntry oldEntry;
        this->index.seekg(oldOffset, std::ios::beg);
        if (this->index.read((char*)&oldEntry, sizeof(oldEntry))) {
            oldEntry.isTombstone = true;
            this->index.seekp(oldOffset, std::ios::beg);
            this->index.write((const char*)&oldEntry, sizeof(oldEntry));
            this->tombstoneOffsets.push_back(oldOffset);
        }

        this->index.seekp(0, std::ios::end);
        std::streamoff newOffset = this->index.tellp();
        this->index.write((const char*)&entry, sizeof(entry));
        this->canFlush = true;
        this->memoryTree[keyStr] = newOffset;
        return;
    }

    std::streamoff insertPosition;

    if (!this->tombstoneOffsets.empty()) {
        insertPosition = this->tombstoneOffsets.back();
        this->tombstoneOffsets.pop_back();
        ZestLog(LogLevel::DEBUG, "IndexManager::insert - reusing tombstone slot");
    } else {
        this->index.seekp(0, std::ios::end);
        insertPosition = this->index.tellp();
    }

    this->index.seekp(insertPosition, std::ios::beg);
    this->index.write((const char*)&entry, sizeof(entry));
    this->canFlush = true;
    this->memoryTree[keyStr] = insertPosition;
}

std::vector<IndexEntry> IndexManager::getAll(unsigned int limit)
{
    std::vector<IndexEntry> res;
    std::shared_lock<std::shared_mutex> lock(this->mtx);

    res.reserve(this->memoryTree.size());

    IndexEntry e;
    for (auto const& [key, offset] : this->memoryTree) {
        if (res.size() >= limit)
            break;

        this->index.seekg(offset, std::ios::beg);

        if (this->index.read((char*)&e, sizeof(IndexEntry))) {
            res.push_back(e);
        } else {
            ZestLog(LogLevel::ERROR, std::format("IndexManager::getAll - Failed to read entry at offset: {}", offset));
            this->index.clear();
        }
    }

    return res;
}

std::vector<IndexEntry> IndexManager::compact()
{
    std::filesystem::copy_file(this->settings.IndexPath, this->settings.DbPath / "INDEX.tmp", std::filesystem::copy_options::overwrite_existing);

    std::vector<IndexEntry> entries = this->getAll();
    std::vector<IndexEntry> validEntries = entries;

    std::unique_lock<std::shared_mutex> lock(this->mtx);

    this->index.close();
    this->index.open(this->indexPath, std::ios::out | std::ios::in | std::ios::binary | std::ios::trunc);

    if (!this->index.is_open()) {
        ZestLog(LogLevel::ERROR, "Compact failed: could not reopen index file.");
        return validEntries;
    }

    this->memoryTree.clear();
    this->tombstoneOffsets.clear();

    std::vector<IndexEntry> result;

    for (const auto& entry : entries) {
        if (entry.isTombstone || entry.segmentId == -1) {
            continue;
        }
        std::streamoff newPos = this->index.tellp();

        if (this->index.write((const char*)&entry, sizeof(IndexEntry))) {
            this->memoryTree[std::string(entry.key)] = newPos;
            result.push_back(entry);
        } else {
            ZestLog(LogLevel::ERROR, std::format("Compact - Failed to write entry for key: {}", std::string(entry.key)));
        }
    }

    this->canFlush = true;
    this->flush();
    ZestLog(LogLevel::DEBUG, "Index compaction completed successfully.");

    std::filesystem::remove(this->settings.DbPath / "INDEX.tmp");
    return result;
}

void IndexManager::flush()
{
    std::unique_lock<std::shared_mutex> lock(this->mtx, std::try_to_lock);
    if (!lock.owns_lock()) {
        ZestLog(LogLevel::DEBUG, "IndexManager::flush - could not acquire lock, skipping");
        return;
    }

    ZestLog(LogLevel::DEBUG, "IndexManager::flush - Flushing to disk...");

    if (this->canFlush) {
        this->index.flush();
        // this->index.fsync();
        this->canFlush = false;
        ZestLog(LogLevel::DEBUG, "IndexManager::flush - Flushing successful");
        return;
    }

    ZestLog(LogLevel::DEBUG, "IndexManager::flush - Flushing skipped, the index is not ready to be flushed");
}