#pragma once

#include <atomic>
#include <fstream>
#include <mutex>
#include <thread>

#include "Settings.hpp"

class DataSegment {
public:
    DataSegment(const Settings& settings, int segmentId);
    ~DataSegment();
    unsigned long write(const std::string& value);
    std::string read(unsigned long offset, unsigned int size);
    bool isFull() const;
    int getSegmentId() const;
    unsigned long getWritePosition() const;
    void refreshFullStatus();

private:
    void openSegment();
    int segmentId;
    std::atomic<unsigned long> currentOffset;
    Settings settings;
    std::fstream segment;
    std::mutex mtx;
    std::thread flushThread;
};