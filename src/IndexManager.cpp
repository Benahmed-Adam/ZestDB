#include "IndexManager.hpp"
#include "Logger.hpp"
#include <chrono>

IndexManager::IndexManager(const Settings& set)
    : settings(set), canFlush(false)
{
    ZestLog(LogLevel::INFO, "Opening INDEX file...");
    this->indexPath = settings.IndexPath;

    this->index.open(this->indexPath, std::ios::in | std::ios::out | std::ios::binary);
    if (!this->index.is_open()) {
        this->index.open(this->indexPath, std::ios::out | std::ios::binary);
        this->index.close();
        this->index.open(this->indexPath, std::ios::in | std::ios::out | std::ios::binary);
    }

    this->loadIndexIntoMemory();
}

IndexManager::~IndexManager()
{
    this->settings.isRunning = false;
    if (this->index.is_open()) {
        this->index.close();
    }
}

void IndexManager::loadIndexIntoMemory()
{
    std::lock_guard<std::mutex> lock(this->mtx);
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
    ZestLog(LogLevel::DEBUG, "IndexManager::search - searching for key: " + key);
    std::lock_guard<std::mutex> lock(this->mtx);

    auto it = this->memoryTree.find(key);
    if (it != this->memoryTree.end()) {
        std::streamoff offset = it->second;

        this->index.seekg(offset, std::ios::beg);
        IndexEntry entry;

        if (this->index.read((char*)&entry, sizeof(entry)) && !entry.isTombstone) {
            ZestLog(LogLevel::DEBUG, "IndexManager::search - found key: " + key);
            return entry;
        }
    }

    ZestLog(LogLevel::DEBUG, "IndexManager::search - key not found: " + key);
    return { "", -1, 0, 0, false };
}

void IndexManager::update(const std::string& key, const IndexEntry& entry)
{
    ZestLog(LogLevel::DEBUG, "IndexManager::update - updating key: " + key);
    std::lock_guard<std::mutex> lock(this->mtx);

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

        ZestLog(LogLevel::DEBUG, "IndexManager::update - key updated at offset: " + std::to_string(offset));
    } else {
        ZestLog(LogLevel::WARNING, "IndexManager::update - key not found for update: " + key);
    }
}

void IndexManager::insert(const IndexEntry& entry)
{
    std::string keyStr(entry.key);
    ZestLog(LogLevel::DEBUG, "IndexManager::insert - inserting key: " + keyStr);
    std::lock_guard<std::mutex> lock(this->mtx);

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

std::vector<IndexEntry> IndexManager::getAll()
{
    std::vector<IndexEntry> res;
    std::lock_guard<std::mutex> lock(this->mtx);

    this->index.seekg(0, std::ios::end);
    std::streamoff fsize = this->index.tellg();

    std::streamoff position = 0;
    IndexEntry e;

    while (position < fsize) {
        this->index.seekg(position, std::ios::beg);
        if (!this->index.read((char*)&e, sizeof(e)))
            break;

        if (!e.isTombstone && e.segmentId != -1) {
            res.push_back(e);
        }
        position += static_cast<std::streamoff>(sizeof(IndexEntry));
    }

    return res;
}

std::vector<IndexEntry> IndexManager::compact()
{
    std::vector<IndexEntry> entries = this->getAll();
    std::vector<IndexEntry> validEntries = entries;

    std::lock_guard<std::mutex> lock(this->mtx);

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
            ZestLog(LogLevel::ERROR, "Compact - Failed to write entry for key: " + std::string(entry.key));
        }
    }

    this->canFlush = true;
    ZestLog(LogLevel::DEBUG, "Index compaction completed successfully.");
    return result;
}

void IndexManager::flush() {
    std::lock_guard<std::mutex> lock(this->mtx);
    ZestLog(LogLevel::DEBUG, "IndexManager::flush - Flushing to disk...");
    
    if (this->canFlush) {
        this->index.flush();
        this->canFlush = false;
        ZestLog(LogLevel::DEBUG, "IndexManager::flush - Flushing successful");
        return;
    }

    ZestLog(LogLevel::DEBUG, "IndexManager::flush - Flushing skipped, the index is not ready to be flushed");
}