#include "StorageManager.hpp"

#include <charconv>
#include <format>
#include <memory>
#include <mutex>
#include <optional>
#include <string_view>
#include <unordered_set>
#include <zstd.h>

#include "Logger.hpp"

namespace Zest {

    namespace {
        constexpr int COMPRESSION_LEVEL = 3;
        constexpr size_t MIN_COMPRESSED_SIZE = 16;                       // a zstd frame alone takes about that: shorter values never shrink
        constexpr unsigned long long MAX_DECOMPRESSED_SIZE = 1ULL << 30; // a corrupted frame header must not trigger a huge allocation
        constexpr std::string_view ZSTD_MAGIC("\x28\xB5\x2F\xFD", 4);

        bool isZstdFrame(std::string_view stored) {
            return stored.starts_with(ZSTD_MAGIC);
        }

        /// The bytes to store for `value`: a zstd frame when it is smaller, otherwise the value as is. A value that
        /// already starts like a zstd frame is always compressed, so that every stored frame is one written here.
        std::optional<std::string> compressValue(const std::string& value) {
            const bool mustCompress = isZstdFrame(value);
            if (value.size() > MAX_DECOMPRESSED_SIZE) {
                // decompressValue() would refuse the frame: keep the value readable by storing it as is
                return mustCompress ? std::nullopt : std::optional<std::string>(value);
            }
            if (value.size() < MIN_COMPRESSED_SIZE && !mustCompress) {
                return value;
            }
            thread_local const std::unique_ptr<ZSTD_CCtx, decltype(&ZSTD_freeCCtx)> ctx = [] {
                std::unique_ptr<ZSTD_CCtx, decltype(&ZSTD_freeCCtx)> created(ZSTD_createCCtx(), &ZSTD_freeCCtx);
                if (created) {
                    ZSTD_CCtx_setParameter(created.get(), ZSTD_c_compressionLevel, COMPRESSION_LEVEL);
                    ZSTD_CCtx_setParameter(created.get(), ZSTD_c_checksumFlag, 1); // a corrupted value fails to decompress instead of reading back wrong
                }
                return created;
            }();
            if (!ctx) {
                ZestLog(LogLevel::Error, "StorageManager - cannot create the zstd compression context");
                return mustCompress ? std::nullopt : std::optional<std::string>(value);
            }
            std::string out(ZSTD_compressBound(value.size()), '\0');
            const size_t size = ZSTD_compress2(ctx.get(), out.data(), out.size(), value.data(), value.size());
            if (ZSTD_isError(size)) {
                ZestLog(LogLevel::Error, std::format("StorageManager - compression failed: {}", ZSTD_getErrorName(size)));
                return mustCompress ? std::nullopt : std::optional<std::string>(value);
            }
            if (size >= value.size() && !mustCompress) {
                return value;
            }
            out.resize(size);
            return out;
        }

        /// The value behind `stored`, or an empty string if it is a frame that cannot be decompressed.
        std::string decompressValue(std::string stored) {
            if (!isZstdFrame(stored)) {
                return stored;
            }
            const unsigned long long size = ZSTD_getFrameContentSize(stored.data(), stored.size());
            if (size == ZSTD_CONTENTSIZE_ERROR || size == ZSTD_CONTENTSIZE_UNKNOWN || size == 0 || size > MAX_DECOMPRESSED_SIZE) {
                ZestLog(LogLevel::Error, "StorageManager - invalid zstd frame header");
                return "";
            }
            thread_local const std::unique_ptr<ZSTD_DCtx, decltype(&ZSTD_freeDCtx)> ctx(ZSTD_createDCtx(), &ZSTD_freeDCtx);
            if (!ctx) {
                ZestLog(LogLevel::Error, "StorageManager - cannot create the zstd decompression context");
                return "";
            }
            std::string out(static_cast<size_t>(size), '\0');
            const size_t got = ZSTD_decompressDCtx(ctx.get(), out.data(), out.size(), stored.data(), stored.size());
            if (ZSTD_isError(got) || got != out.size()) {
                ZestLog(LogLevel::Error, std::format("StorageManager - decompression failed: {}", ZSTD_isError(got) ? ZSTD_getErrorName(got) : "size mismatch"));
                return "";
            }
            return out;
        }
    } // namespace

    StorageManager::StorageManager(Settings& set)
        : settings(set) {
        this->latestSegmentId = 0;
        this->boot();
    }

    void StorageManager::boot() {
        int nb = 0;
        for (auto& entry : std::filesystem::directory_iterator(this->settings.DbPath / "seg")) {
            if (entry.path().extension() != ".seg") {
                continue;
            }
            std::string stem = entry.path().stem().string();
            uint32_t segId;
            auto [end, ec] = std::from_chars(stem.data(), stem.data() + stem.size(), segId);
            if (ec != std::errc() || end != stem.data() + stem.size()) {
                ZestLog(LogLevel::Warning, std::format("StorageManager - Ignoring unexpected file: {}", entry.path().string()));
                continue;
            }
            this->segments[segId] = std::make_unique<DataSegment>(this->settings, segId);
            nb++;
        }

        if (nb == 0) {
            this->segments[1] = std::make_unique<DataSegment>(this->settings, 1);
            this->latestSegmentId = 1;
        } else {
            uint32_t maxId = 0;
            for (const auto& [id, seg] : this->segments) {
                if (id > maxId)
                    maxId = id;
            }
            this->latestSegmentId = maxId;
        }
    }

    IndexEntry StorageManager::appendToSegment(DataSegment* seg, const std::string& value) {
        unsigned long pos = seg->write(value);

        if (pos == this->settings.SegSize + 1) {
            return { "", INVALID_OFFSET, INVALID_SEGMENT_ID, 0, false };
        }

        return { "", static_cast<uint64_t>(pos), seg->getSegmentId(), static_cast<uint32_t>(value.size()), false };
    }

    IndexEntry StorageManager::append(const std::string& value) {
        // A value too large for a segment stays refused even when its compressed form would fit
        if (value.size() > this->settings.SegSize) {
            return { "", INVALID_OFFSET, INVALID_SEGMENT_ID, 0, false };
        }
        const std::optional<std::string> stored = compressValue(value);
        if (!stored) {
            return { "", INVALID_OFFSET, INVALID_SEGMENT_ID, 0, false };
        }
        return this->appendStored(*stored);
    }

    IndexEntry StorageManager::appendStored(const std::string& value) {
        std::lock_guard<std::mutex> lock(this->mtx);

        uint32_t currentId = this->latestSegmentId.load();
        DataSegment* seg = nullptr;

        auto it = this->segments.find(currentId);
        if (it != this->segments.end() && !it->second->isFull()) {
            seg = it->second.get();
        } else {
            uint32_t nextId = currentId + 1;

            this->segments[nextId] = std::make_unique<DataSegment>(this->settings, nextId);
            this->latestSegmentId.store(nextId);
            seg = this->segments[nextId].get();
        }

        IndexEntry result = this->appendToSegment(seg, value);

        // The segment was not full yet, but this value does not fit in what is left of it: start a new segment.
        // A value is never split across two segments, so the end of the previous one stays unused.
        if (result.segmentId == INVALID_SEGMENT_ID) {
            ZestLog(LogLevel::Debug, std::format("StorageManager::append - segment {} full, creating new segment", seg->getSegmentId()));
            uint32_t nextId = this->latestSegmentId.load() + 1;

            this->segments[nextId] = std::make_unique<DataSegment>(this->settings, nextId);
            this->latestSegmentId.store(nextId);
            result = this->appendToSegment(this->segments[nextId].get(), value);
        }

        return result;
    }

    bool StorageManager::contains(const IndexEntry& entry) const {
        auto it = this->segments.find(entry.segmentId);
        return it != this->segments.end() && entry.offset + entry.size <= it->second->getWritePosition();
    }

    std::string StorageManager::read(const IndexEntry& entry) {
        auto it = this->segments.find(entry.segmentId);
        if (it != this->segments.end()) {
            // An offset is always below SegSize, an unsigned long (32 bits on Windows)
            return decompressValue(it->second->read(static_cast<unsigned long>(entry.offset), entry.size));
        }

        ZestLog(LogLevel::Error, std::format("StorageManager::read - segment not found: {}", entry.segmentId));
        return "";
    }

    void StorageManager::removeUnusedSegments(const std::vector<uint32_t>& usedSegmentIds) {
        std::lock_guard<std::mutex> lock(this->mtx);
        std::unordered_set<uint32_t> usedSet(usedSegmentIds.begin(), usedSegmentIds.end());

        std::vector<uint32_t> toRemove;
        for (const auto& [id, seg] : this->segments) {
            if (usedSet.find(id) == usedSet.end()) {
                toRemove.push_back(id);
            }
        }

        for (uint32_t id : toRemove) {
            auto it = this->segments.find(id);
            if (it != this->segments.end()) {
                std::filesystem::path segPath = this->settings.DbPath / "seg" / std::format("{}.seg", id);
                it = this->segments.erase(it);

                if (std::filesystem::exists(segPath)) {
                    if (!std::filesystem::remove(segPath)) {
                        ZestLog(LogLevel::Error, std::format("StorageManager - Failed to remove file: {}", segPath.string()));
                    }
                }
            }
        }
    }

    bool StorageManager::flush() {
        bool ok = true;
        for (auto& [id, seg] : this->segments) {
            ok = seg->flush() && ok;
        }
        return ok;
    }

    uint32_t StorageManager::currentSegmentId() const {
        return this->latestSegmentId.load();
    }

} // namespace Zest
