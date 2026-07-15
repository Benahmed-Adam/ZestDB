#include "DataSegment.hpp"
#include "Logger.hpp"
#include <chrono>
#include <format>

namespace Zest {

DataSegment::DataSegment(Settings& set, int id)
    : segmentId(id)
    , currentOffset(0)
    , settings(set)
    , canFlush(false)
{
    ZestLog(LogLevel::DEBUG, std::format("DataSegment::DataSegment - creating segment: {}", id));
    this->openSegment();
    this->refreshFullStatus();
}

DataSegment::~DataSegment()
{
    if (this->segment.is_open()) {
        this->segment.close();
    }
}

void DataSegment::openSegment()
{
    std::filesystem::path segPath = this->settings.DbPath / "seg" / std::format("{}.seg", this->segmentId);

    bool fileExists = std::filesystem::exists(segPath);

    if (fileExists) {
        this->segment.open(segPath, std::ios::in | std::ios::out | std::ios::binary);
    } else {
        this->segment.open(segPath, std::ios::out | std::ios::binary | std::ios::trunc);
        this->segment.close();
        this->segment.open(segPath, std::ios::in | std::ios::out | std::ios::binary);
    }

    if (!this->segment.is_open()) {
        ZestLog(LogLevel::ERROR, std::format("DataSegment::DataSegment - failed to open segment: {}", segPath.string()));
    }
}

void DataSegment::refreshFullStatus()
{
    this->segment.seekg(0, std::ios::end);
    std::streamoff pos = this->segment.tellg();
    this->segment.seekp(0, std::ios::end);
    this->segment.tellp();
    this->currentOffset = static_cast<unsigned long>(pos);
}

unsigned long DataSegment::getWritePosition() const
{
    return this->currentOffset.load();
}

unsigned long DataSegment::write(const std::string& value)
{
    std::lock_guard<std::mutex> lock(this->mtx);

    unsigned long startOffset = this->currentOffset.load();

    if (startOffset + value.size() > this->settings.SegSize) {
        ZestLog(LogLevel::DEBUG, std::format("DataSegment::write - segment {} would exceed capacity", this->segmentId));
        return this->settings.SegSize + 1;
    }

    this->segment.seekp(static_cast<std::streamoff>(startOffset), std::ios::beg);

    if (!this->segment.good()) {
        ZestLog(LogLevel::ERROR, "DataSegment::write - seek failed");
        return this->settings.SegSize + 1;
    }

    this->segment.write(value.c_str(), static_cast<std::streamsize>(value.size()));
    this->canFlush = true;

    if (!this->segment.good()) {
        ZestLog(LogLevel::ERROR, "DataSegment::write - write failed");
        return this->settings.SegSize + 1;
    }

    unsigned long newOffset = startOffset + value.size();
    this->currentOffset.store(newOffset);

    if (newOffset >= this->settings.SegSize) {
        ZestLog(LogLevel::DEBUG, std::format("DataSegment::write - segment {} is now full", this->segmentId));
    }

    ZestLog(LogLevel::DEBUG, std::format("DataSegment::write - wrote {} bytes at offset: {}", value.size(), startOffset));
    return startOffset;
}

std::string DataSegment::read(unsigned long offset, unsigned int size)
{
    std::lock_guard<std::mutex> lock(this->mtx);
    ZestLog(LogLevel::DEBUG, std::format("DataSegment::read - reading {} bytes from offset: {}", size, offset));

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
    return this->currentOffset.load() >= this->settings.SegSize;
}

int DataSegment::getSegmentId() const
{
    return this->segmentId;
}

void DataSegment::flush()
{
    std::unique_lock<std::mutex> lock(this->mtx, std::try_to_lock);
    if (!lock.owns_lock()) {
        ZestLog(LogLevel::DEBUG, "DataSegment::flush - could not acquire lock, skipping");
        return;
    }

    ZestLog(LogLevel::DEBUG, "DataSegment::flush - Flushing to disk...");

    if (this->canFlush) {
        this->segment.flush();
        // this->segment.fsync();
        this->canFlush = false;
        ZestLog(LogLevel::DEBUG, "DataSegment::flush - Flushing successful");
        return;
    }

    ZestLog(LogLevel::DEBUG, "DataSegment::flush - Flushing skipped, the segment is not ready to be flushed");
}

} // namespace Zest
