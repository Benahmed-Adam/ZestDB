#pragma once

#include <filesystem>
#include <fstream>
#include <functional>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "Settings.hpp"

namespace Zest {

    static constexpr size_t MAX_KEY_SIZE = 256;
    static constexpr uint32_t INVALID_SEGMENT_ID = UINT32_MAX;
    static constexpr uint64_t INVALID_OFFSET = UINT64_MAX;

    struct IndexEntry {
        std::string key;
        uint64_t offset;
        uint32_t segmentId;
        uint32_t size;
        bool isTombstone;

        void serialize(std::ostream& os) const {
            uint32_t klen = static_cast<uint32_t>(key.size());

#pragma pack(push, 1)

            struct Header {
                uint32_t klen;
                uint64_t offset;
                uint32_t segmentId;
                uint32_t size;
                bool isTombstone;
            };

#pragma pack(pop)

            Header header{ klen, offset, segmentId, size, isTombstone };

            os.write(reinterpret_cast<const char*>(&header), sizeof(header));

            if (klen > 0) {
                os.write(key.data(), klen);
            }
        }

        bool deserialize(std::istream& is) {
#pragma pack(push, 1)

            struct Header {
                uint32_t klen;
                uint64_t offset;
                uint32_t segmentId;
                uint32_t size;
                bool isTombstone;
            };

#pragma pack(pop)

            Header header;
            if (!is.read(reinterpret_cast<char*>(&header), sizeof(header)) || header.klen > MAX_KEY_SIZE) {
                return false;
            }

            offset = header.offset;
            segmentId = header.segmentId;
            size = header.size;
            isTombstone = header.isTombstone;

            key.resize(header.klen);
            if (header.klen > 0) {
                if (!is.read(&key[0], header.klen))
                    return false;
            }

            return true;
        }
    };

    class IndexManager {
    public:
        IndexManager(Settings& set);
        ~IndexManager();

        IndexEntry search(const std::string& key);
        void update(const std::string& key, const IndexEntry& entry);
        void insert(const IndexEntry& entry);
        std::vector<IndexEntry> getAll(unsigned int limit = UINT_MAX, const std::function<bool()>& stopEarly = {});
        std::vector<IndexEntry> findMatching(const std::function<bool(std::string_view)>& matches, const std::function<bool()>& stopEarly = {});
        std::vector<std::string> keys();
        std::vector<IndexEntry> compact();
        bool flush();

    private:
        struct Location {
            uint64_t offset;
            uint32_t segmentId;
            uint32_t size;
        };

        std::filesystem::path indexPath;
        std::fstream index;
        std::mutex mtx;
        Settings& settings;

        // Every live entry is kept in memory: lookups never read the INDEX file, which is only appended to
        std::unordered_map<std::string, Location> memoryTree;

        bool canFlush;

        void loadIndexIntoMemory();
        void append(const IndexEntry& entry);
        static IndexEntry toEntry(const std::string& key, const Location& loc);
    };

} // namespace Zest
