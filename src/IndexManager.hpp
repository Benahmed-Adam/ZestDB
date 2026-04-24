#pragma once

#include <filesystem>
#include <fstream>
#include <unordered_map>
#include <mutex>
#include <string>
#include <vector>

#include "Settings.hpp"

struct IndexEntry {
    static constexpr size_t MAX_KEY_SIZE = 256;
    char key[MAX_KEY_SIZE];
    int segmentId;
    unsigned long offset;
    unsigned int size;
    bool isTombstone;
};

class IndexManager {
public:
    IndexManager(const Settings& set);
    ~IndexManager();

    IndexEntry search(const std::string& key);
    void update(const std::string& key, const IndexEntry& entry);
    void insert(const IndexEntry& entry);
    void markAsTombstone(const std::string& key);
    std::vector<IndexEntry> getAll();
    std::vector<IndexEntry> compact();
    void flush();
private:
    std::filesystem::path indexPath;
    std::fstream index;
    std::mutex mtx;
    Settings settings;

    std::unordered_map<std::string, std::streamoff> memoryTree;
    std::vector<std::streamoff> tombstoneOffsets;

    bool canFlush;

    void loadIndexIntoMemory();
};