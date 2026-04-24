#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "DataSegment.hpp"
#include "IndexManager.hpp"
#include "Settings.hpp"

class StorageManager {
public:
    StorageManager(const Settings& set);
    IndexEntry append(const std::string& value);
    std::string read(const IndexEntry& entry);
    void removeUnusedSegments(const std::vector<int>& usedSegmentIds);
    void flush();
private:
    void boot();
    IndexEntry appendToSegment(DataSegment* seg, const std::string& value);
    std::vector<std::unique_ptr<DataSegment>> segments;
    std::atomic<int> latestSegmentId;
    Settings settings;
    std::mutex segmentsMtx;
};
