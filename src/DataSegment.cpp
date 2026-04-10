#include "DataSegment.hpp"
#include "Logger.hpp"

DataSegment::DataSegment(const Settings& set, int id)
    : segmentId(id)
    , full(false)
{
    ZestLog(LogLevel::DEBUG, "DataSegment::DataSegment - creating segment: " + std::to_string(id));
    std::filesystem::path segPath = set.DbPath / "seg" / (std::to_string(id) + ".seg");
    this->segment.open(segPath, std::ios::in | std::ios::out | std::ios::binary | std::ios::app);
    if (!this->segment.is_open()) {
        ZestLog(LogLevel::DEBUG, "DataSegment::DataSegment - creating new segment file: " + segPath.string());
        this->segment.open(segPath, std::ios::out | std::ios::binary | std::ios::trunc);
        this->segment.close();
        this->segment.open(segPath, std::ios::in | std::ios::out | std::ios::binary | std::ios::app);
    }
    this->settings = set;
}

DataSegment::~DataSegment()
{
    this->segment.close();
}

unsigned long DataSegment::write(const std::string& value)
{
    std::lock_guard<std::mutex> lock(this->mtx);

    this->segment.seekp(0, std::ios::end);
    unsigned long position = static_cast<unsigned long>(this->segment.tellp());

    if (position >= this->settings.SegSize || position + value.size() >= this->settings.SegSize) {
        ZestLog(LogLevel::DEBUG, "DataSegment::write - segment " + std::to_string(segmentId) + " is full");
        this->full = true;
        return this->settings.SegSize + 1;
    }

    this->segment.write(value.c_str(), static_cast<std::streamsize>(value.size()));
    this->segment.flush();

    ZestLog(LogLevel::DEBUG, "DataSegment::write - wrote " + std::to_string(value.size()) + " bytes at offset: " + std::to_string(position));
    return position;
}

std::string DataSegment::read(unsigned long offset, unsigned int size)
{
    ZestLog(LogLevel::DEBUG, "DataSegment::read - reading " + std::to_string(size) + " bytes from offset: " + std::to_string(offset));
    std::string res;
    res.resize(size);

    std::lock_guard<std::mutex> lock(this->mtx);

    this->segment.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
    this->segment.read(&res[0], size);

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