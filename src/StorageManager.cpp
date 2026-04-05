#include "StorageManager.hpp"
#include <iostream>

StorageManager::StorageManager(const Settings& s)
{
    this->settings = s;
    this->boot();
}

void StorageManager::boot()
{
    int nb = 0;
    for (auto& entry : std::filesystem::directory_iterator(this->settings.DbPath / "seg")) {
        std::string ext = entry.path().extension();
        if (ext == ".seg") {
            this->segments.push_back(std::make_unique<DataSegment>(this->settings, std::stoi(entry.path().filename())));
            nb++;
        }
    }

    if (nb == 0) {
        this->segments.push_back(std::make_unique<DataSegment>(this->settings, 1));
    }
}

IndexEntry StorageManager::append(const std::string& value)
{
    auto* currentSeg = this->segments.back().get();

    unsigned long pos = currentSeg->write(value);

    if (pos == 128001) {
        int nextId = currentSeg->getSegmentId() + 1;

        this->segments.push_back(std::make_unique<DataSegment>(this->settings, nextId));

        currentSeg = this->segments.back().get();
        pos = currentSeg->write(value);
    }

    return { "", currentSeg->getSegmentId(), pos, (unsigned int)value.size(), false };
}

std::string StorageManager::read(const IndexEntry& entry)
{
    for (auto& segPtr : this->segments) {
        if (segPtr->getSegmentId() == entry.segmentId) {
            return segPtr->read(entry.offset, entry.size);
        }
    }

    return "nope";
}