#pragma once

#include <memory>
#include <mutex>
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
    void boot();
    std::vector<std::unique_ptr<DataSegment>> segments;
    Settings settings;
    std::mutex mtx;
};
