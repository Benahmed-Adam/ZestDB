#pragma once

#include <string>
#include <vector>

#include "DataSegment.hpp"
#include "IndexManager.hpp"
#include "Settings.hpp"

class StorageManager {
public:
    StorageManager(const Settings& settings);
    IndexEntry append(const std::string& value);
    std::string read(const IndexEntry& entry);

private:
    void createNewSegment();
    std::vector<DataSegment> segments;
    Settings settings;
};
