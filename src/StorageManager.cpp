#include <iostream>
#include <mutex>

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
            this->segments.push_back(std::make_unique<DataSegment>(this->settings, std::stoi(entry.path().filename())));
            nb++;
        }
    }

    if (nb == 0) {
        ZestLog(LogLevel::DEBUG, "StorageManager::boot - no segments found, creating segment 1");
        this->segments.push_back(std::make_unique<DataSegment>(this->settings, 1));
        this->latestSegmentId = 1;
    } else {
        std::sort(this->segments.begin(), this->segments.end(),
            [](const std::unique_ptr<DataSegment>& a, const std::unique_ptr<DataSegment>& b) {
                return a->getSegmentId() < b->getSegmentId();
            });
        this->latestSegmentId = this->segments.back()->getSegmentId();
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

    while (true) {
        int currentId = this->latestSegmentId.load();

        {
            std::lock_guard<std::mutex> lock(this->segmentsMtx);
            for (auto& segPtr : this->segments) {
                if (segPtr->getSegmentId() == currentId && !segPtr->isFull()) {
                    IndexEntry entry = this->appendToSegment(segPtr.get(), value);
                    if (entry.segmentId != -1) {
                        return entry;
                    }
                }
            }
        }

        int nextId = currentId + 1;
        if (this->latestSegmentId.compare_exchange_weak(currentId, nextId)) {
            std::lock_guard<std::mutex> lock(this->segmentsMtx);
            ZestLog(LogLevel::DEBUG, "StorageManager::append - creating new segment: " + std::to_string(nextId));
            this->segments.push_back(std::make_unique<DataSegment>(this->settings, nextId));

            DataSegment* newSeg = this->segments.back().get();
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

    std::lock_guard<std::mutex> lock(this->segmentsMtx);

    for (auto& segPtr : this->segments) {
        if (segPtr->getSegmentId() == entry.segmentId) {
            return segPtr->read(entry.offset, entry.size);
        }
    }

    ZestLog(LogLevel::ERROR, "StorageManager::read - segment not found: " + std::to_string(entry.segmentId));
    return "";
}

void StorageManager::removeUnusedSegments(const std::vector<int>& usedSegmentIds)
{
    std::lock_guard<std::mutex> lock(this->segmentsMtx);

    for (auto it = this->segments.begin(); it != this->segments.end();) {
        int id = (*it)->getSegmentId();
        bool used = std::find(usedSegmentIds.begin(), usedSegmentIds.end(), id) != usedSegmentIds.end();

        if (!used) {
            std::string segPath = (this->settings.DbPath / "seg" / (std::to_string(id) + ".seg")).string();
            if (std::remove(segPath.c_str()) != 0) {
                ZestLog(LogLevel::ERROR, "StorageManager::removeUnusedSegments - Failed to remove segment file: " + segPath);
            } else {
                ZestLog(LogLevel::DEBUG, "StorageManager::removeUnusedSegments - Removed segment file: " + segPath);
            }
            it = this->segments.erase(it);
        } else {
            ++it;
        }
    }

    ZestLog(LogLevel::DEBUG, "StorageManager::removeUnusedSegments - Removed unused segments, remaining: " + std::to_string(this->segments.size()));
}

void StorageManager::flush() {
    ZestLog(LogLevel::DEBUG, "StorageManager::flush - Flushing each DataSegment");
    for (auto& seg : this->segments) {
        seg->flush();
    }
    ZestLog(LogLevel::DEBUG, "Storagemanager::flush - DataSegments successfully flushed");
}