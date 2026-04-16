#include "IndexManager.hpp"
#include "Logger.hpp"

IndexManager::IndexManager(const Settings& settings)
{
    ZestLog(LogLevel::INFO, "Opening INDEX file...");
    this->indexPath = settings.IndexPath;
    this->index.open(this->indexPath, std::ios::in | std::ios::out | std::ios::binary);
    if (!this->index.is_open()) {
        this->index.open(this->indexPath, std::ios::out | std::ios::binary);
        this->index.close();
        this->index.open(this->indexPath, std::ios::in | std::ios::out | std::ios::binary);
    }
}

IndexManager::~IndexManager()
{
    if (this->index.is_open()) {
        this->index.close();
    }
}

IndexEntry IndexManager::search(const std::string& key)
{
    ZestLog(LogLevel::DEBUG, "IndexManager::search - searching for key: " + key);

    std::lock_guard<std::mutex> lock(this->mtx);

    this->index.seekg(0, std::ios::end);
    std::streamoff fsize = this->index.tellg();

    std::streamoff position = 0;
    IndexEntry entry;
    IndexEntry foundEntry = { "", -1, 0, 0, false };
    bool found = false;

    while (position < fsize) {
        this->index.seekg(position, std::ios::beg);
        if (!this->index.read((char*)&entry, sizeof(entry)))
            break;

        std::string entryKey(entry.key);
        if (entryKey == key && !entry.isTombstone) {
            if (!found) {
                foundEntry = entry;
                found = true;
            }
        }
        position += static_cast<std::streamoff>(sizeof(IndexEntry));
    }

    if (found) {
        ZestLog(LogLevel::DEBUG, "IndexManager::search - found key: " + key + " in segment: " + std::to_string(foundEntry.segmentId));
        return foundEntry;
    }

    ZestLog(LogLevel::DEBUG, "IndexManager::search - key not found: " + key);
    return { "", -1, 0, 0, false };
}

void IndexManager::update(const std::string& key, const IndexEntry& entry)
{
    ZestLog(LogLevel::DEBUG, "IndexManager::update - updating key: " + key);

    std::lock_guard<std::mutex> lock(this->mtx);

    this->index.seekg(0, std::ios::end);
    std::streamoff fsize = this->index.tellg();

    std::streamoff position = 0;
    IndexEntry e;
    bool updated = false;

    while (position < fsize) {
        this->index.seekg(position, std::ios::beg);
        if (!this->index.read((char*)&e, sizeof(e)))
            break;

        std::string eKey(e.key);
        if (eKey == key && !e.isTombstone) {
            this->index.seekp(position, std::ios::beg);
            this->index.write((char*)&entry, sizeof(entry));
            this->index.flush();
            ZestLog(LogLevel::DEBUG, "IndexManager::update - key updated: " + key);
            updated = true;
            break;
        }
        position += static_cast<std::streamoff>(sizeof(IndexEntry));
    }

    if (!updated) {
        ZestLog(LogLevel::WARNING, "IndexManager::update - key not found for update: " + key);
    }
}

void IndexManager::insert(const IndexEntry& entry)
{
    std::string keyStr(entry.key);
    ZestLog(LogLevel::DEBUG, "IndexManager::insert - inserting key: " + keyStr);

    std::lock_guard<std::mutex> lock(this->mtx);

    this->index.seekg(0, std::ios::end);
    std::streamoff fsize = this->index.tellg();

    std::streamoff position = 0;
    IndexEntry e;
    std::streamoff insertPosition = -1;

    while (position < fsize) {
        this->index.seekg(position, std::ios::beg);
        if (!this->index.read((char*)&e, sizeof(e)))
            break;

        std::string eKey(e.key);
        if (eKey == keyStr && !e.isTombstone) {
            ZestLog(LogLevel::DEBUG, "IndexManager::insert - key exists, updating: " + keyStr);
            this->index.seekp(position, std::ios::beg);
            this->index.write((char*)&entry, sizeof(entry));
            this->index.flush();
            return;
        }

        if (eKey == keyStr && e.isTombstone && insertPosition == -1) {
            insertPosition = position;
        }

        position += static_cast<std::streamoff>(sizeof(IndexEntry));
    }

    if (insertPosition != -1) {
        this->index.seekp(insertPosition, std::ios::beg);
        this->index.write((char*)&entry, sizeof(entry));
        this->index.flush();
        ZestLog(LogLevel::DEBUG, "IndexManager::insert - reusing tombstone slot for key: " + keyStr);
    } else {
        this->index.seekp(0, std::ios::end);
        this->index.write((char*)&entry, sizeof(entry));
        this->index.flush();
        ZestLog(LogLevel::DEBUG, "IndexManager::insert - inserted new key: " + keyStr);
    }
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