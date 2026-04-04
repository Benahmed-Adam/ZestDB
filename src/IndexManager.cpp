#include "IndexManager.hpp"
#include "Logger.hpp"

IndexManager::IndexManager(Settings& settings)
{
    ZestLog(LogLevel::INFO, "Opening INDEX file...");
    this->indexPath = settings.IndexPath;
    this->index.open(this->indexPath, std::ios::in | std::ios::out | std::ios::binary);
}

IndexManager::~IndexManager()
{
    this->index.close();
}

IndexEntry IndexManager::search(std::string& key)
{
    index.seekg(0, std::ios::end);
    std::streamoff fsize = index.tellg();

    std::streamoff position = 0;
    IndexEntry entry;

    while (position < fsize) {
        this->index.seekg(position, std::ios::beg);
        if (!this->index.read((char*)&entry, sizeof(entry)) && !entry.isTombstone)
            break;

        if (std::string(entry.key) == key) {
            return entry;
        }
        position += static_cast<std::streamoff>(sizeof(IndexEntry));
    }

    return { "", -1, 0, 0, 0 };
}

void IndexManager::update(std::string& key, IndexEntry& entry)
{
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
            return;
        }
        position += static_cast<std::streamoff>(sizeof(IndexEntry));
    }
}

void IndexManager::insert(IndexEntry& entry)
{
    this->index.seekp(0, std::ios::end);
    this->index.write((char*)&entry, sizeof(entry));
    this->index.flush();
}