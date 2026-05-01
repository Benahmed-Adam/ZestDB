#include <format>
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
        ZestLog(LogLevel::DEBUG, std::format("StorageManager::boot - found {} segments, latest: {}", nb, this->latestSegmentId.load()));
    }
}

IndexEntry StorageManager::appendToSegment(DataSegment* seg, const std::string& value)
{
    unsigned long pos = seg->write(value);

    if (pos == this->settings.SegSize + 1) {
        ZestLog(LogLevel::DEBUG, "StorageManager::appendToSegment - segment full");
        return { "", -1, 0, 0, false };
    }

    ZestLog(LogLevel::DEBUG, std::format("StorageManager::appendToSegment - written to segment: {} at offset: {}", seg->getSegmentId(), pos));
    return { "", seg->getSegmentId(), pos, (unsigned int)value.size(), false };
}

IndexEntry StorageManager::append(const std::string& value)
{
    std::lock_guard<std::mutex> lock(this->mtx);

    int currentId = this->latestSegmentId.load();
    DataSegment* seg = nullptr;

    auto it = this->segments.find(currentId);
    if (it != this->segments.end() && !it->second->isFull()) {
        seg = it->second.get();
    } else {
        int nextId = currentId + 1;
        ZestLog(LogLevel::DEBUG, std::format("Creating new segment: {}", nextId));

        this->segments[nextId] = std::make_unique<DataSegment>(this->settings, nextId);
        this->latestSegmentId.store(nextId);
        seg = this->segments[nextId].get();
    }

    return this->appendToSegment(seg, value);
}

std::string StorageManager::read(const IndexEntry& entry)
{
    ZestLog(LogLevel::DEBUG, std::format("StorageManager::read - segment: {}, offset: {}, size: {}", entry.segmentId, entry.offset, entry.size));

    auto it = this->segments.find(entry.segmentId);
    if (it != this->segments.end()) {
        return it->second->read(entry.offset, entry.size);
    }

    ZestLog(LogLevel::ERROR, std::format("StorageManager::read - segment not found: {}", entry.segmentId));
    return "";
}

void StorageManager::removeUnusedSegments(const std::vector<int>& usedSegmentIds)
{
    std::lock_guard<std::mutex>(this->mtx);
    std::unordered_set<int> usedSet(usedSegmentIds.begin(), usedSegmentIds.end());

    for (auto it = this->segments.begin(); it != this->segments.end();) {
        int id = it->first;

        if (usedSet.find(id) == usedSet.end()) {
            std::filesystem::path segPath = this->settings.DbPath / "seg" / std::format("{}.seg", id);

            it = this->segments.erase(it);

            if (std::filesystem::exists(segPath)) {
                if (std::filesystem::remove(segPath)) {
                    ZestLog(LogLevel::DEBUG, std::format("StorageManager - Removed segment file: {}", segPath.string()));
                } else {
                    ZestLog(LogLevel::ERROR, std::format("StorageManager - Failed to remove file: {}", segPath.string()));
                }
            }
        } else {
            ++it;
        }
    }

    ZestLog(LogLevel::DEBUG, std::format("StorageManager - Remaining segments in memory: {}", this->segments.size()));
}

void StorageManager::flush()
{
    ZestLog(LogLevel::DEBUG, "StorageManager::flush - Flushing each DataSegment");
    for (auto& [id, seg] : this->segments) {
        seg->flush();
    }
    ZestLog(LogLevel::DEBUG, "Storagemanager::flush - DataSegments successfully flushed");
}