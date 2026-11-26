#include "IndexManager.hpp"

#include <chrono>
#include <cstring>
#include <format>
#include <iterator>
#include <string_view>

#include "Checksum.hpp"
#include "File.hpp"
#include "Logger.hpp"

namespace Zest {

    namespace {
        constexpr std::string_view INDEX_MAGIC("ZIDX\x01", 5); // file header: format name and version

#pragma pack(push, 1)

        /// Record header, followed by the key bytes. `crc` covers everything after it: the rest of the header and the key.
        struct RecordHeader {
            uint32_t crc;
            uint32_t klen;
            uint64_t offset;
            uint32_t segmentId;
            uint32_t size;
            uint8_t flags;
            uint64_t expiresAt;
            uint32_t valueCrc;
        };

#pragma pack(pop)

        constexpr uint8_t FLAG_TOMBSTONE = 1;

        std::string encodeRecord(const IndexEntry& entry) {
            const RecordHeader header{ 0, static_cast<uint32_t>(entry.key.size()), entry.offset, entry.segmentId, entry.size, static_cast<uint8_t>(entry.isTombstone ? FLAG_TOMBSTONE : 0), entry.expiresAt, entry.valueCrc };
            std::string out(sizeof(header), '\0');
            std::memcpy(out.data(), &header, sizeof(header));
            out += entry.key;
            const uint32_t crc = crc32Of(std::string_view(out).substr(sizeof(header.crc)));
            std::memcpy(out.data(), &crc, sizeof(crc));
            return out;
        }

        /// Decodes the record at `pos` and moves `pos` past it. False if it is cut short, has an impossible key
        /// length or flags, or fails its checksum.
        bool decodeRecord(std::string_view data, size_t& pos, IndexEntry& out) {
            RecordHeader header;
            if (data.size() - pos < sizeof(header)) {
                return false;
            }
            std::memcpy(&header, data.data() + pos, sizeof(header));
            if (header.klen > MAX_KEY_SIZE || data.size() - pos - sizeof(header) < header.klen || (header.flags & ~FLAG_TOMBSTONE) != 0) {
                return false;
            }
            if (crc32Of(data.substr(pos + sizeof(header.crc), sizeof(header) - sizeof(header.crc) + header.klen)) != header.crc) {
                return false;
            }
            out = { std::string(data.substr(pos + sizeof(header), header.klen)), header.offset, header.segmentId, header.size, (header.flags & FLAG_TOMBSTONE) != 0, header.expiresAt, header.valueCrc };
            pos += sizeof(header) + header.klen;
            return true;
        }

    } // namespace

    IndexManager::IndexManager(Settings& set)
        : settings(set),
          canFlush(false) {
        ZestLog(LogLevel::Info, "Opening INDEX file...");
        this->indexPath = set.IndexPath;

        std::filesystem::remove(this->settings.DbPath / "INDEX.tmp");

        this->index.open(this->indexPath, std::ios::in | std::ios::out | std::ios::binary);
        if (!this->index.is_open()) {
            this->index.open(this->indexPath, std::ios::out | std::ios::binary);
            this->index.close();
            this->index.open(this->indexPath, std::ios::in | std::ios::out | std::ios::binary);
        }

        this->loadIndexIntoMemory();
        this->compact();
    }

    IndexManager::~IndexManager() {
        if (this->index.is_open()) {
            this->index.close();
        }
    }

    IndexEntry IndexManager::toEntry(const std::string& key, const Location& loc) {
        return { key, loc.offset, loc.segmentId, loc.size, false, loc.expiresAt, loc.valueCrc };
    }

    IndexManager::Location IndexManager::toLocation(const IndexEntry& entry) {
        return { entry.offset, entry.segmentId, entry.size, entry.expiresAt, entry.valueCrc };
    }

    void IndexManager::loadIndexIntoMemory() {
        std::unique_lock<std::mutex> lock(this->mtx);
        this->index.clear();
        this->index.seekg(0, std::ios::beg);
        const std::string data((std::istreambuf_iterator<char>(this->index)), std::istreambuf_iterator<char>());
        this->index.clear();

        if (data.empty()) {
            this->index.seekp(0, std::ios::end);
            this->index.write(INDEX_MAGIC.data(), static_cast<std::streamsize>(INDEX_MAGIC.size()));
            this->canFlush = true;
        } else {
            if (!data.starts_with(INDEX_MAGIC)) {
                ZestLog(LogLevel::Warning, std::format("IndexManager - {} has a damaged file header, reading its records anyway", this->indexPath.string()));
            }
            size_t pos = INDEX_MAGIC.size();
            size_t goodEnd = pos;
            size_t skipped = 0;
            size_t pendingSkip = 0;
            while (pos < data.size()) {
                IndexEntry entry;
                size_t next = pos;
                if (!decodeRecord(data, next, entry)) {
                    pos++;
                    pendingSkip++;
                    continue;
                }
                skipped += pendingSkip;
                pendingSkip = 0;
                if (entry.isTombstone) {
                    this->memoryTree.erase(entry.key);
                } else {
                    this->memoryTree[entry.key] = toLocation(entry);
                }
                pos = next;
                goodEnd = next;
            }

            if (skipped > 0) {
                ZestLog(LogLevel::Error, std::format("IndexManager - Skipped {} corrupted bytes in {}: the records they held are lost", skipped, this->indexPath.string()));
            }
            if (goodEnd < data.size()) {
                ZestLog(LogLevel::Warning, std::format("IndexManager - Truncating {} unreadable bytes at the end of the INDEX", data.size() - goodEnd));
                this->index.close();
                std::filesystem::resize_file(this->indexPath, static_cast<std::uintmax_t>(goodEnd));
                this->index.open(this->indexPath, std::ios::in | std::ios::out | std::ios::binary);
            }
        }
        this->index.seekp(0, std::ios::end);
        this->recomputeLiveSize();
        ZestLog(LogLevel::Info, "IndexManager - Loaded entries into memory tree.");
    }

    void IndexManager::recomputeLiveSize() {
        uint64_t total = 0;
        for (const auto& [key, loc] : this->memoryTree) {
            total += loc.size;
        }
        this->liveSize = total;
    }

    void IndexManager::append(const IndexEntry& entry) {
        this->index.clear();
        const std::string record = encodeRecord(entry);
        this->index.write(record.data(), static_cast<std::streamsize>(record.size()));
        this->canFlush = true;
    }

    IndexEntry IndexManager::search(const std::string& key) {
        std::unique_lock<std::mutex> lock(this->mtx);

        auto it = this->memoryTree.find(key);
        if (it != this->memoryTree.end() && !toEntry(it->first, it->second).isExpired()) {
            return toEntry(it->first, it->second);
        }

        return { "", INVALID_OFFSET, INVALID_SEGMENT_ID, 0, false };
    }

    void IndexManager::update(const std::string& key, const IndexEntry& entry) {
        std::unique_lock<std::mutex> lock(this->mtx);

        auto it = this->memoryTree.find(key);
        if (it == this->memoryTree.end()) {
            return;
        }

        IndexEntry record = entry;
        record.key = key;
        this->append(record);
        this->liveSize -= it->second.size;
        if (entry.isTombstone) {
            this->memoryTree.erase(it);
        } else {
            it->second = toLocation(entry);
            this->liveSize += entry.size;
        }
    }

    void IndexManager::insert(const IndexEntry& entry) {
        std::unique_lock<std::mutex> lock(this->mtx);
        this->append(entry);
        const Location loc = toLocation(entry);
        auto [it, inserted] = this->memoryTree.try_emplace(entry.key, loc);
        if (!inserted) {
            this->liveSize -= it->second.size;
            it->second = loc;
        }
        this->liveSize += entry.size;
    }

    std::vector<IndexEntry> IndexManager::getAll(unsigned int limit, const std::function<bool()>& stopEarly) {
        std::vector<IndexEntry> res;
        std::unique_lock<std::mutex> lock(this->mtx);

        res.reserve(std::min<size_t>(limit, this->memoryTree.size()));
        const uint64_t now = nowMs();
        for (const auto& [key, loc] : this->memoryTree) {
            if (res.size() >= limit)
                break;
            if (stopEarly && stopEarly())
                break;
            IndexEntry entry = toEntry(key, loc);
            if (!entry.isExpired(now)) {
                res.push_back(std::move(entry));
            }
        }

        return res;
    }

    std::vector<IndexEntry> IndexManager::findMatching(const std::function<bool(std::string_view)>& matches, const std::function<bool()>& stopEarly) {
        std::vector<IndexEntry> res;
        std::unique_lock<std::mutex> lock(this->mtx);

        const uint64_t now = nowMs();
        for (const auto& [key, loc] : this->memoryTree) {
            if (stopEarly && stopEarly())
                break;
            if (matches(key) && !toEntry(key, loc).isExpired(now)) {
                res.push_back(toEntry(key, loc));
            }
        }

        return res;
    }

    size_t IndexManager::size() {
        std::unique_lock<std::mutex> lock(this->mtx);
        return this->memoryTree.size();
    }

    size_t IndexManager::count(const std::function<bool(std::string_view)>& matches) {
        std::unique_lock<std::mutex> lock(this->mtx);

        const uint64_t now = nowMs();
        size_t n = 0;
        for (const auto& [key, loc] : this->memoryTree) {
            const bool expired = loc.expiresAt != 0 && loc.expiresAt <= now;
            if (!expired && (!matches || matches(key))) {
                n++;
            }
        }
        return n;
    }

    std::vector<std::string> IndexManager::keys(const std::function<bool(std::string_view)>& matches) {
        std::unique_lock<std::mutex> lock(this->mtx);
        std::vector<std::string> res;
        const uint64_t now = nowMs();
        for (const auto& [key, loc] : this->memoryTree) {
            const bool expired = loc.expiresAt != 0 && loc.expiresAt <= now;
            if (!expired && (!matches || matches(key))) {
                res.push_back(key);
            }
        }
        return res;
    }

    std::vector<IndexEntry> IndexManager::compact() {
        ZestLog(LogLevel::Debug, "Starting index compaction...");

        std::vector<IndexEntry> entries = this->getAll();

        std::unique_lock<std::mutex> lock(this->mtx);

        const std::filesystem::path tmpPath = this->settings.DbPath / "INDEX.tmp";
        std::fstream tmp(tmpPath, std::ios::out | std::ios::in | std::ios::binary | std::ios::trunc);
        tmp.write(INDEX_MAGIC.data(), static_cast<std::streamsize>(INDEX_MAGIC.size()));
        for (const auto& entry : entries) {
            const std::string record = encodeRecord(entry);
            tmp.write(record.data(), static_cast<std::streamsize>(record.size()));
        }

        std::error_code ec;
        if (!tmp.good() || !flush_and_fsync(tmp, tmpPath)) {
            ZestLog(LogLevel::Error, "Compact failed: could not write INDEX.tmp, keeping the current INDEX");
            tmp.close();
            std::filesystem::remove(tmpPath, ec);
            return entries;
        }
        tmp.close();

        this->index.close();
        std::filesystem::rename(tmpPath, this->indexPath, ec);
        this->index.open(this->indexPath, std::ios::in | std::ios::out | std::ios::binary);
        this->index.seekp(0, std::ios::end);
        if (ec || !this->index.is_open()) {
            ZestLog(LogLevel::Error, std::format("Compact failed: could not replace the INDEX: {}", ec.message()));
            return entries;
        }

        this->canFlush = false;
        std::erase_if(this->memoryTree, [now = nowMs()](const auto& item) { return toEntry(item.first, item.second).isExpired(now); });
        this->recomputeLiveSize();
        ZestLog(LogLevel::Debug, "Index compaction completed successfully.");
        return entries;
    }

    bool IndexManager::flush() {
        std::unique_lock<std::mutex> lock(this->mtx);

        if (this->canFlush) {
            if (!flush_and_fsync(this->index, this->indexPath)) {
                return false;
            }
            this->canFlush = false;
        }
        return true;
    }

} // namespace Zest
