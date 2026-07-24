#pragma once

#include <atomic>
#include <fstream>
#include <mutex>
#include <thread>

#include "Settings.hpp"

namespace Zest {

    class DataSegment {
    public:
        DataSegment(Settings &set, uint32_t segmentId);
        ~DataSegment();
        unsigned long write(const std::string &value);
        std::string read(unsigned long offset, unsigned int size);
        bool isFull() const;
        uint32_t getSegmentId() const;
        unsigned long getWritePosition() const;
        void refreshFullStatus();
        void flush();

    private:
        void openSegment();
        uint32_t segmentId;
        std::atomic<unsigned long> currentOffset;
        Settings &settings;
        std::fstream segment;
        std::mutex mtx;

        bool canFlush;
    };

} // namespace Zest
