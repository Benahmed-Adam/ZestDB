#include "DataSegment.hpp"
#include "Logger.hpp"

DataSegment::DataSegment(const Settings& set, int id)
    : segmentId(id)
    , full(false)
    , settings(set)
{
    ZestLog(LogLevel::DEBUG, "DataSegment::DataSegment - creating segment: " + std::to_string(id));
    std::filesystem::path segPath = set.DbPath / "seg" / (std::to_string(id) + ".seg");

    bool fileExists = std::filesystem::exists(segPath);

    if (fileExists) {
        this->segment.open(segPath, std::ios::in | std::ios::out | std::ios::binary);
    } else {
        this->segment.open(segPath, std::ios::out | std::ios::binary | std::ios::trunc);
        this->segment.close();
        this->segment.open(segPath, std::ios::in | std::ios::out | std::ios::binary);
    }

    if (!this->segment.is_open()) {
        ZestLog(LogLevel::ERROR, "DataSegment::DataSegment - failed to open segment: " + segPath.string());
    }

    this->checkFull();
}

DataSegment::~DataSegment()
{
    if (this->segment.is_open()) {
        this->segment.close();
    }
}

void DataSegment::checkFull()
{
    this->segment.seekp(0, std::ios::end);
    std::streamoff pos = this->segment.tellp();
    if (static_cast<unsigned long>(pos) >= this->settings.SegSize) {
        this->full = true;
    }
}

unsigned long DataSegment::write(const std::string& value)
{
    std::lock_guard<std::mutex> lock(this->mtx);

    if (this->full) {
        ZestLog(LogLevel::DEBUG, "DataSegment::write - segment " + std::to_string(segmentId) + " is full");
        return this->settings.SegSize + 1;
    }

    this->segment.seekp(0, std::ios::end);
    unsigned long position = static_cast<unsigned long>(this->segment.tellp());

    if (position + value.size() > this->settings.SegSize) {
        ZestLog(LogLevel::DEBUG, "DataSegment::write - segment " + std::to_string(segmentId) + " would be full after write");
        this->full = true;
        return this->settings.SegSize + 1;
    }

    this->segment.write(value.c_str(), static_cast<std::streamsize>(value.size()));
    this->segment.flush();

    if (static_cast<unsigned long>(this->segment.tellp()) >= this->settings.SegSize) {
        this->full = true;
    }

    ZestLog(LogLevel::DEBUG, "DataSegment::write - wrote " + std::to_string(value.size()) + " bytes at offset: " + std::to_string(position));
    return position;
}

std::string DataSegment::read(unsigned long offset, unsigned int size)
{
    ZestLog(LogLevel::DEBUG, "DataSegment::read - reading " + std::to_string(size) + " bytes from offset: " + std::to_string(offset));

    std::lock_guard<std::mutex> lock(this->mtx);

    this->segment.seekg(static_cast<std::streamoff>(offset), std::ios::beg);

    if (!this->segment.good()) {
        ZestLog(LogLevel::ERROR, "DataSegment::read - seek failed");
        return "";
    }

    std::string res;
    res.resize(size);

    this->segment.read(&res[0], size);

    if (!this->segment.good() && !this->segment.eof()) {
        ZestLog(LogLevel::ERROR, "DataSegment::read - read failed");
        return "";
    }

    res.resize(static_cast<std::string::size_type>(this->segment.gcount()));
    return res;
}

bool DataSegment::isFull() const
{
    return this->full;
}

int DataSegment::getSegmentId() const
{
    return this->segmentId;
}