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

        void serialize(std::ostream &os) const {
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

            os.write(reinterpret_cast<const char *>(&header), sizeof(header));

            if (klen > 0) {
                os.write(key.data(), klen);
            }
        }

        bool deserialize(std::istream &is) {
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
            if (!is.read(reinterpret_cast<char *>(&header), sizeof(header))) {
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
        IndexManager(Settings &set);
        ~IndexManager();

        IndexEntry search(const std::string &key);
        void update(const std::string &key, const IndexEntry &entry);
        void insert(const IndexEntry &entry);
        std::vector<IndexEntry> getAll(unsigned int limit = UINT_MAX, const std::function<bool()> &stopEarly = {});
        std::vector<IndexEntry> compact();
        void flush();

    private:
        std::filesystem::path indexPath;
        std::fstream index;
        std::shared_mutex mtx;
        Settings &settings;

        std::unordered_map<std::string, std::streamoff> memoryTree;
        std::vector<std::streamoff> tombstoneOffsets;

        bool canFlush;

        void loadIndexIntoMemory();
    };

} // namespace Zest
