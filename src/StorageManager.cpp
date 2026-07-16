#include "StorageManager.hpp"

#include <format>
#include <mutex>
#include <unordered_set>

#include "Logger.hpp"

namespace Zest {

    StorageManager::StorageManager(Settings &set)
        : settings(set) {
        this->latestSegmentId = 0;
        this->boot();
    }

    void StorageManager::boot() {
        int nb = 0;
        for (auto &entry : std::filesystem::directory_iterator(this->settings.DbPath / "seg")) {
            std::string ext = entry.path().extension();
            if (ext == ".seg") {
                int segId = std::stoi(entry.path().filename());
                this->segments[segId] = std::make_unique<DataSegment>(this->settings, segId);
                nb++;
            }
        }

        if (nb == 0) {
            this->segments[1] = std::make_unique<DataSegment>(this->settings, 1);
            this->latestSegmentId = 1;
        } else {
            int maxId = 0;
            for (const auto &[id, seg] : this->segments) {
                if (id > maxId)
                    maxId = id;
            }
            this->latestSegmentId = maxId;
        }
    }

    IndexEntry StorageManager::appendToSegment(DataSegment *seg, const std::string &value) {
        unsigned long pos = seg->write(value);

        if (pos == this->settings.SegSize + 1) {
            return { "", -1, 0, 0, false };
        }

        return { "", seg->getSegmentId(), pos, (unsigned int)value.size(), false };
    }

    IndexEntry StorageManager::append(const std::string &value) {
        std::lock_guard<std::mutex> lock(this->mtx);

        int currentId = this->latestSegmentId.load();
        DataSegment *seg = nullptr;

        auto it = this->segments.find(currentId);
        if (it != this->segments.end() && !it->second->isFull()) {
            seg = it->second.get();
        } else {
            int nextId = currentId + 1;

            this->segments[nextId] = std::make_unique<DataSegment>(this->settings, nextId);
            this->latestSegmentId.store(nextId);
            seg = this->segments[nextId].get();
        }

        return this->appendToSegment(seg, value);
    }

    std::string StorageManager::read(const IndexEntry &entry) {
        auto it = this->segments.find(entry.segmentId);
        if (it != this->segments.end()) {
            return it->second->read(entry.offset, entry.size);
        }

        ZestLog(LogLevel::ERROR, std::format("StorageManager::read - segment not found: {}", entry.segmentId));
        return "";
    }

    void StorageManager::removeUnusedSegments(const std::vector<int> &usedSegmentIds) {
        std::lock_guard<std::mutex> lock(this->mtx);
        std::unordered_set<int> usedSet(usedSegmentIds.begin(), usedSegmentIds.end());

        std::vector<int> toRemove;
        for (const auto &[id, seg] : this->segments) {
            if (usedSet.find(id) == usedSet.end()) {
                toRemove.push_back(id);
            }
        }

        for (int id : toRemove) {
            auto it = this->segments.find(id);
            if (it != this->segments.end()) {
                std::filesystem::path segPath = this->settings.DbPath / "seg" / std::format("{}.seg", id);
                it = this->segments.erase(it);

                if (std::filesystem::exists(segPath)) {
                    if (!std::filesystem::remove(segPath)) {
                        ZestLog(LogLevel::ERROR, std::format("StorageManager - Failed to remove file: {}", segPath.string()));
                    }
                }
            }
        }
    }

    void StorageManager::flush() {
        for (auto &[id, seg] : this->segments) {
            seg->flush();
        }
    }

} // namespace Zest
