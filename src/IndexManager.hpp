#pragma once

#include <filesystem>
#include <fstream>

#include "Settings.hpp"

struct IndexEntry {
    char key[64];
    int segmentId;
    long offset;
    unsigned int size;
    bool isTombstone;
};

class IndexManager {
public:
    IndexManager(Settings& settings);
    ~IndexManager();
    IndexEntry search(std::string& key);
    void update(std::string& key, IndexEntry& entry);
    void insert(IndexEntry& key);
private:
    std::filesystem::path indexPath;
    std::fstream index;
};