#pragma once

#include <fstream>
#include <mutex>

#include "Settings.hpp"

class DataSegment {
public:
    DataSegment(const Settings& settings, int segmentId);
    ~DataSegment();
    unsigned long write(const std::string& value);
    std::string read(unsigned long offset, unsigned int size);
    bool isFull() const;
    int getSegmentId() const;

private:
    void checkFull();
    int segmentId;
    bool full;
    Settings settings;
    std::fstream segment;
    std::mutex mtx;
};