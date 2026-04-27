#include <iostream>
#include <mutex>
#include <unordered_set>

#include "Logger.hpp"
#include "StorageManager.hpp"

StorageManager::StorageManager(const Settings& set)
{
    ZestLog(LogLevel::DEBUG, "Initializing StorageManager...");
    this->settings = set;
    this->latestSegmentId = 0;
    this->boot();
    ZestLog(LogLevel::DEBUG, "StorageManager initialized");
}

void StorageManager::boot()
{
    ZestLog(LogLevel::DEBUG, "StorageManager::boot - scanning segments...");
    int nb = 0;
    for (auto& entry : std::filesystem::directory_iterator(this->settings.DbPath / "seg")) {
        std::string ext = entry.path().extension();
        if (ext == ".seg") {
            int segId = std::stoi(entry.path().filename());
            this->segments[segId] = std::make_unique<DataSegment>(this->settings, segId);
            nb++;
        }
    }

    if (nb == 0) {
        ZestLog(LogLevel::DEBUG, "StorageManager::boot - no segments found, creating segment 1");
        this->segments[1] = std::make_unique<DataSegment>(this->settings, 1);
        this->latestSegmentId = 1;
    } else {
        int maxId = 0;
        for (const auto& [id, seg] : this->segments) {
            if (id > maxId)
                maxId = id;
        }
        this->latestSegmentId = maxId;
        ZestLog(LogLevel::DEBUG, "StorageManager::boot - found " + std::to_string(nb) + " segments, latest: " + std::to_string(this->latestSegmentId.load()));
    }
}

IndexEntry StorageManager::appendToSegment(DataSegment* seg, const std::string& value)
{
    unsigned long pos = seg->write(value);

    if (pos == this->settings.SegSize + 1) {
        ZestLog(LogLevel::DEBUG, "StorageManager::appendToSegment - segment full");
        return { "", -1, 0, 0, false };
    }

    ZestLog(LogLevel::DEBUG, "StorageManager::appendToSegment - written to segment: " + std::to_string(seg->getSegmentId()) + " at offset: " + std::to_string(pos));
    return { "", seg->getSegmentId(), pos, (unsigned int)value.size(), false };
}

IndexEntry StorageManager::append(const std::string& value)
{
    ZestLog(LogLevel::DEBUG, "StorageManager::append - writing value of size: " + std::to_string(value.size()));

    std::lock_guard<std::mutex> lock(this->mtx);

    while (true) {
        int currentId = this->latestSegmentId.load();

        auto it = this->segments.find(currentId);
        if (it != this->segments.end() && !it->second->isFull()) {
            IndexEntry entry = this->appendToSegment(it->second.get(), value);
            if (entry.segmentId != -1) {
                return entry;
            }
        }

        int nextId = currentId + 1;
        if (this->latestSegmentId.compare_exchange_weak(currentId, nextId)) {
            ZestLog(LogLevel::DEBUG, "StorageManager::append - creating new segment: " + std::to_string(nextId));
            this->segments[nextId] = std::make_unique<DataSegment>(this->settings, nextId);

            DataSegment* newSeg = this->segments[nextId].get();
            IndexEntry entry = this->appendToSegment(newSeg, value);
            if (entry.segmentId != -1) {
                return entry;
            }
        }

        currentId = this->latestSegmentId.load();
    }
}

std::string StorageManager::read(const IndexEntry& entry)
{
    ZestLog(LogLevel::DEBUG, "StorageManager::read - segment: " + std::to_string(entry.segmentId) + ", offset: " + std::to_string(entry.offset) + ", size: " + std::to_string(entry.size));

    auto it = this->segments.find(entry.segmentId);
    if (it != this->segments.end()) {
        return it->second->read(entry.offset, entry.size);
    }

    ZestLog(LogLevel::ERROR, "StorageManager::read - segment not found: " + std::to_string(entry.segmentId));
    return "";
}

void StorageManager::removeUnusedSegments(const std::vector<int>& usedSegmentIds)
{
    std::lock_guard<std::mutex>(this->mtx);
    std::unordered_set<int> usedSet(usedSegmentIds.begin(), usedSegmentIds.end());

    for (auto it = this->segments.begin(); it != this->segments.end();) {
        int id = it->first;
        
        if (usedSet.find(id) == usedSet.end()) {
            std::filesystem::path segPath = this->settings.DbPath / "seg" / (std::to_string(id) + ".seg");

            it = this->segments.erase(it); 

            if (std::filesystem::exists(segPath)) {
                if (std::filesystem::remove(segPath)) {
                    ZestLog(LogLevel::DEBUG, "StorageManager - Removed segment file: " + segPath.string());
                } else {
                    ZestLog(LogLevel::ERROR, "StorageManager - Failed to remove file: " + segPath.string());
                }
            }
        } else {
            ++it;
        }
    }

    ZestLog(LogLevel::DEBUG, "StorageManager - Remaining segments in memory: " + std::to_string(this->segments.size()));
}

void StorageManager::flush()
{
    ZestLog(LogLevel::DEBUG, "StorageManager::flush - Flushing each DataSegment");
    for (auto& [id, seg] : this->segments) {
        seg->flush();
    }
    ZestLog(LogLevel::DEBUG, "Storagemanager::flush - DataSegments successfully flushed");
}