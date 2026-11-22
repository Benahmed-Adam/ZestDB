#pragma once

#include <atomic>
#include <fstream>
#include <mutex>
#include <thread>

#include "Settings.hpp"

namespace Zest {

    /// One append-only value file, `<DbPath>/seg/<id>.seg`.
    /// Values are written one after the other and never modified; the index records where each one starts and how long it is.
    /// A segment is full once its size reaches `SegSize`, and the StorageManager then opens the next one.
    class DataSegment {
    public:
        /// Opens (or creates) the segment file and positions the write offset at its end.
        DataSegment(Settings& set, uint32_t segmentId);
        ~DataSegment();

        /// Appends `value` at the end of the segment.
        /// @return the offset where it starts, or `SegSize + 1` when it does not fit or the write failed.
        unsigned long write(const std::string& value);

        /// Reads `size` bytes at `offset`.
        /// @return the bytes, or an empty string when the read fails or comes back short.
        std::string read(unsigned long offset, unsigned int size);

        /// True once the segment holds `SegSize` bytes or more.
        bool isFull() const;

        uint32_t getSegmentId() const;

        /// Current size of the segment, which is where the next value will be written.
        unsigned long getWritePosition() const;

        /// Re-reads the file size into the write offset (after opening, or when `SegSize` changed).
        void refreshFullStatus();

        /// fsyncs the file if something was written since the last flush.
        /// @return false if the data could not be synced.
        bool flush();

    private:
        /// Opens the file for reading and writing, creating it first if needed.
        void openSegment();

        uint32_t segmentId;
        std::atomic<unsigned long> currentOffset;
        Settings& settings;
        std::fstream segment;
        std::filesystem::path path;
        std::mutex mtx; // reads and writes share one stream position

        bool canFlush;           // something was written since the last fsync
        bool atWriteEnd = false; // the put position is already at the end: consecutive writes skip the seek
    };

} // namespace Zest
