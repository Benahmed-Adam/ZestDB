#include "DataSegment.hpp"

DataSegment::DataSegment(const Settings& settings, int segmentId)
    : segmentId(segmentId)
    , full(false)
{
    this->segment.open(settings.DbPath / "seg" / (std::to_string(segmentId) + ".seg"), std::ios::in | std::ios::out | std::ios::binary);
}

DataSegment::~DataSegment() {
    this->segment.close();
}

unsigned long DataSegment::write(const std::string& value) {
    this->segment.seekp(0, std::ios::end);
    unsigned long position = this->segment.tellp();

    if (position > 128000) {
        this->full = true;
        return 128001;
    }

    this->segment.write(value.c_str(), value.size());
    this->segment.flush();

    return position;
}

std::string DataSegment::read(unsigned long offset, unsigned int size) {
    std::string res;
    res.resize(size);

    this->segment.seekg(offset, std::ios::beg);
    this->segment.read(&res[0], size);

    return res;
}

bool DataSegment::isFull() const {
    return this->full;
}