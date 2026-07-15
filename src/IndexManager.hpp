#pragma once

#include <filesystem>
#include <fstream>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "Settings.hpp"

namespace Zest {

    static constexpr size_t MAX_KEY_SIZE = 256;

    struct IndexEntry {
        char key[MAX_KEY_SIZE];
        int segmentId;
        unsigned long offset;
        unsigned int size;
        bool isTombstone;
    };

    class IndexManager {
    public:
        IndexManager(Settings& set);
        ~IndexManager();

        IndexEntry search(const std::string& key);
        void update(const std::string& key, const IndexEntry& entry);
        void insert(const IndexEntry& entry);
        std::vector<IndexEntry> getAll(unsigned int limit = UINT_MAX);
        std::vector<IndexEntry> compact();
        void flush();

    private:
        std::filesystem::path indexPath;
        std::fstream index;
        std::shared_mutex mtx;
        Settings& settings;

        std::unordered_map<std::string, std::streamoff> memoryTree;
        std::vector<std::streamoff> tombstoneOffsets;

        bool canFlush;

        void loadIndexIntoMemory();
    };

} // namespace Zest
