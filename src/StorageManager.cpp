#include "StorageManager.hpp"
#include "Logger.hpp"
#include <iostream>
#include <mutex>

StorageManager::StorageManager(const Settings& s)
{
    ZestLog(LogLevel::DEBUG, "Initializing StorageManager...");
    this->settings = s;
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
        ZestLog(LogLevel::INFO, "StorageManager::boot - no segments found, creating segment 1");
        this->segments.push_back(std::make_unique<DataSegment>(this->settings, 1));
    } else {
        std::sort(this->segments.begin(), this->segments.end(),
            [](const std::unique_ptr<DataSegment>& a, const std::unique_ptr<DataSegment>& b) {
                return a->getSegmentId() < b->getSegmentId();
            });
        ZestLog(LogLevel::DEBUG, "StorageManager::boot - found " + std::to_string(nb) + " segments");
    }
}

IndexEntry StorageManager::append(const std::string& value)
{
    ZestLog(LogLevel::DEBUG, "StorageManager::append - writing value of size: " + std::to_string(value.size()));

    std::lock_guard<std::mutex> lock(this->mtx);

    DataSegment* currentSeg = this->segments.back().get();

    unsigned long pos = currentSeg->write(value);

    if (pos == this->settings.SegSize + 1) {
        ZestLog(LogLevel::DEBUG, "StorageManager::append - segment full, creating new segment");
        int nextId = currentSeg->getSegmentId() + 1;

        this->segments.push_back(std::make_unique<DataSegment>(this->settings, nextId));

        currentSeg = this->segments.back().get();
        pos = currentSeg->write(value);

        if (pos == this->settings.SegSize + 1) {
            ZestLog(LogLevel::ERROR, "StorageManager::append - failed to write to new segment");
            return { "", -1, 0, 0, false };
        }
    }

    ZestLog(LogLevel::DEBUG, "StorageManager::append - written to segment: " + std::to_string(currentSeg->getSegmentId()) + " at offset: " + std::to_string(pos));
    return { "", currentSeg->getSegmentId(), pos, (unsigned int)value.size(), false };
}

std::string StorageManager::read(const IndexEntry& entry)
{
    ZestLog(LogLevel::DEBUG, "StorageManager::read - segment: " + std::to_string(entry.segmentId) + ", offset: " + std::to_string(entry.offset) + ", size: " + std::to_string(entry.size));

    std::lock_guard<std::mutex> lock(this->mtx);

    for (auto& segPtr : this->segments) {
        if (segPtr->getSegmentId() == entry.segmentId) {
            return segPtr->read(entry.offset, entry.size);
        }
    }

    ZestLog(LogLevel::ERROR, "StorageManager::read - segment not found: " + std::to_string(entry.segmentId));
    return "";
}