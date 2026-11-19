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

    /// The value files of one shard: a set of DataSegment, only the latest of which receives writes.
    /// It knows nothing about keys; the IndexManager maps each key to the location this class returns.
    class StorageManager {
    public:
        /// Opens every `<DbPath>/seg/<id>.seg` file, or creates segment 1 in an empty folder.
        StorageManager(Settings& set);

        /// Writes `value` to the current segment, opening a new one when it is full.
        /// @return the location (segment, offset, size) with an empty key, or `INVALID_SEGMENT_ID` if the write failed.
        IndexEntry append(const std::string& value);

        /// Reads the value at `entry`'s location, or returns an empty string if the segment does not exist or the read fails.
        std::string read(const IndexEntry& entry);

        /// True if `entry` points inside the written part of an existing segment (used to drop index entries left by a crash).
        bool contains(const IndexEntry& entry) const;

        /// Closes and deletes every segment whose id is not in `usedSegmentIds` (called by compaction).
        void removeUnusedSegments(const std::vector<uint32_t>& usedSegmentIds);

        /// fsyncs every segment that was written to. @return false if one of them failed.
        bool flush();

        /// Id of the segment that receives the writes.
        uint32_t currentSegmentId() const;

    private:
        /// Loads the existing segments and finds the latest one.
        void boot();

        /// Writes to `seg` and turns the offset it returns into a location.
        IndexEntry appendToSegment(DataSegment* seg, const std::string& value);

        std::unordered_map<uint32_t, std::unique_ptr<DataSegment>> segments;
        std::atomic<uint32_t> latestSegmentId;
        std::mutex mtx; // guards the segment map against append and compaction
        Settings& settings;
    };

} // namespace Zest
