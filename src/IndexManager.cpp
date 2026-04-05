#include "IndexManager.hpp"
#include "Logger.hpp"

IndexManager::IndexManager(const Settings& settings)
{
    ZestLog(LogLevel::INFO, "Opening INDEX file...");
    this->indexPath = settings.IndexPath;
    this->index.open(this->indexPath, std::ios::in | std::ios::out | std::ios::binary);
}

IndexManager::~IndexManager()
{
    this->index.close();
}

IndexEntry IndexManager::search(const std::string& key)
{
    ZestLog(LogLevel::DEBUG, "IndexManager::search - searching for key: " + key);
    index.seekg(0, std::ios::end);
    std::streamoff fsize = index.tellg();

    std::streamoff position = 0;
    IndexEntry entry;

    while (position < fsize) {
        this->index.seekg(position, std::ios::beg);
        if (!this->index.read((char*)&entry, sizeof(entry)) && entry.isTombstone)
            break;

        if (std::string(entry.key) == key) {
            ZestLog(LogLevel::DEBUG, "IndexManager::search - found key: " + key + " in segment: " + std::to_string(entry.segmentId));
            return entry;
        }
        position += static_cast<std::streamoff>(sizeof(IndexEntry));
    }

    ZestLog(LogLevel::DEBUG, "IndexManager::search - key not found: " + key);
    return { "", -1, 0, 0, 0 };
}

void IndexManager::update(const std::string& key, const IndexEntry& entry)
{
    ZestLog(LogLevel::DEBUG, "IndexManager::update - updating key: " + key);
    index.seekg(0, std::ios::end);
    std::streamoff fsize = index.tellg();

    std::streamoff position = 0;
    IndexEntry e;

    while (position < fsize) {
        this->index.seekg(position, std::ios::beg);
        if (!this->index.read((char*)&e, sizeof(e)))
            break;

        if (std::string(e.key) == key) {
            this->index.seekp(position, std::ios::beg);
            this->index.write((char*)&entry, sizeof(entry));
            this->index.flush();
            ZestLog(LogLevel::DEBUG, "IndexManager::update - key updated: " + key);
            return;
        }
        position += static_cast<std::streamoff>(sizeof(IndexEntry));
    }
}

void IndexManager::insert(const IndexEntry& entry)
{
    ZestLog(LogLevel::DEBUG, "IndexManager::insert - inserting key: " + std::string(entry.key) + " to segment: " + std::to_string(entry.segmentId));
    this->index.seekp(0, std::ios::end);
    this->index.write((char*)&entry, sizeof(entry));
    this->index.flush();
    ZestLog(LogLevel::DEBUG, "IndexManager::insert - key inserted successfully");
}