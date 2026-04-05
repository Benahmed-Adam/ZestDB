#pragma once

#include <filesystem>
#include <fstream>

#include "Settings.hpp"

struct IndexEntry {
    char key[64];
    int segmentId;
    unsigned long offset;
    unsigned int size;
    bool isTombstone;
};

class IndexManager {
public:
    IndexManager(const Settings& settings);
    ~IndexManager();
    IndexEntry search(const std::string& key);
    void update(const std::string& key, const IndexEntry& entry);
    void insert(const IndexEntry& key);

private:
    std::filesystem::path indexPath;
    std::fstream index;
};