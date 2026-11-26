#pragma once

#include <atomic>
#include <chrono>
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

    /// Hard limit on key length; `MaxKeySize` can only lower it. It also bounds the key length read back from the INDEX file.
    static constexpr size_t MAX_KEY_SIZE = 256;
    /// Segment id of an entry that does not exist (lookup miss, failed write).
    static constexpr uint32_t INVALID_SEGMENT_ID = UINT32_MAX;
    static constexpr uint64_t INVALID_OFFSET = UINT64_MAX;

    /// Current time in milliseconds since the Unix epoch, the unit of every expiry date.
    inline uint64_t nowMs() {
        return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count());
    }

    /// Where a key's value is stored, and when it expires. IndexManager.cpp turns it into an INDEX record and back.
    struct IndexEntry {
        std::string key;
        uint64_t offset;
        uint32_t segmentId;
        uint32_t size;
        bool isTombstone;       // a delete record: the key no longer exists
        uint64_t expiresAt = 0; // absolute date in ms, 0 for no expiry
        uint32_t valueCrc = 0;  // CRC-32 of the bytes stored in the segment

        /// True if the entry has an expiry date that is already past.
        bool isExpired(uint64_t now = nowMs()) const {
            return expiresAt != 0 && expiresAt <= now;
        }
    };

    /// The key index of one shard: every live key and its location, kept in memory, backed by the INDEX file.
    ///
    /// The INDEX file is an append-only log: a write appends the new location, a delete appends a tombstone, and loading
    /// replays the log in order, so the last record of a key wins. compact() rewrites it without the dead records.
    /// The file starts with `ZIDX` and a version byte, and each record carries a CRC-32 of its content.
    /// Lookups and scans only read memory. Expired keys stay in memory until the next compaction, but every read skips them.
    class IndexManager {
    public:
        /// Opens (or creates) the INDEX file, loads it into memory, then compacts it.
        IndexManager(Settings& set);
        ~IndexManager();

        /// Location of `key`, or an entry with `INVALID_SEGMENT_ID` if the key does not exist or has expired.
        IndexEntry search(const std::string& key);

        /// Records a new location for a key that exists (or a tombstone when `entry.isTombstone`); does nothing for an unknown key.
        void update(const std::string& key, const IndexEntry& entry);

        /// Records the location of `entry.key`, whether the key exists or not.
        void insert(const IndexEntry& entry);

        /// Live entries, at most `limit` of them; stops early once `stopEarly()` returns true.
        std::vector<IndexEntry> getAll(unsigned int limit = UINT_MAX, const std::function<bool()>& stopEarly = {});

        /// Live entries whose key satisfies `matches`; stops early once `stopEarly()` returns true.
        std::vector<IndexEntry> findMatching(const std::function<bool(std::string_view)>& matches, const std::function<bool()>& stopEarly = {});

        /// Number of live keys that satisfy `matches` (every live key when it is empty).
        size_t count(const std::function<bool(std::string_view)>& matches = {});

        /// Live keys that satisfy `matches` (every live key when it is empty), in no particular order.
        std::vector<std::string> keys(const std::function<bool(std::string_view)>& matches = {});

        /// Rewrites the INDEX file with one record per live key, through `INDEX.tmp` and a rename, and drops expired keys from memory.
        /// If the new file cannot be written, the current one is kept.
        /// @return the live entries, which tell the shard which segments are still used.
        std::vector<IndexEntry> compact();

        /// fsyncs the INDEX file if records were appended since the last flush. @return false if the sync failed.
        bool flush();

        /// Bytes of segment data the index points to. Expired keys count until the next compaction drops them.
        uint64_t liveBytes() const {
            return this->liveSize.load();
        }

        /// Number of keys in memory, without scanning them: expired keys count until the next compaction drops them.
        size_t size();

    private:
        /// What the index stores per key: an IndexEntry without the key and the tombstone flag.
        struct Location {
            uint64_t offset;
            uint32_t segmentId;
            uint32_t size;
            uint64_t expiresAt;
            uint32_t valueCrc;
        };

        std::filesystem::path indexPath;
        std::fstream index;
        std::mutex mtx;
        Settings& settings;

        std::unordered_map<std::string, Location> memoryTree;

        bool canFlush;                      // records were appended since the last fsync
        std::atomic<uint64_t> liveSize = 0; // sum of the sizes in memoryTree, changed under `mtx`

        /// Replays the INDEX file into memory. A record whose checksum fails is skipped up to the next valid one,
        /// and an unreadable tail (a record cut by a crash) is truncated.
        void loadIndexIntoMemory();

        /// Sets `liveSize` from memoryTree; the caller holds `mtx`.
        void recomputeLiveSize();

        /// Appends a record to the INDEX file; the caller holds `mtx`.
        void append(const IndexEntry& entry);

        static IndexEntry toEntry(const std::string& key, const Location& loc);
        static Location toLocation(const IndexEntry& entry);
    };

} // namespace Zest
