#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "DataSegment.hpp"
#include "IndexManager.hpp"
#include "Settings.hpp"

namespace Zest {

    class StorageManager {
    public:
        StorageManager(Settings &set);
        IndexEntry append(const std::string &value);
        std::string read(const IndexEntry &entry);
        void removeUnusedSegments(const std::vector<int> &usedSegmentIds);
        void flush();

    private:
        void boot();
        IndexEntry appendToSegment(DataSegment *seg, const std::string &value);
        std::unordered_map<int, std::unique_ptr<DataSegment>> segments;
        std::atomic<int> latestSegmentId;
        std::mutex mtx;
        Settings &settings;
    };

} // namespace Zest
