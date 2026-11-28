#pragma once

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <vector>

#include "Settings.hpp"

namespace Zest {

    /// One WAL record: the command to replay and its global sequence number.
    struct WalEntry {
        std::string cmd;
        uint64_t seq;
    };

    /// Write-ahead log of one shard.
    ///
    /// Each write appends the command that reproduces its effect (`s k "v"`, `setexat ...`, `d k`) before it is
    /// acknowledged; on startup the records of every shard are merged by sequence number and replayed. Once the
    /// index and segments are flushed, the WAL is cleared.
    ///
    /// The file starts with `ZWAL` and a version byte. Record format: `uint32 length`, `uint32 crc`, the command,
    /// `uint64 sequence`; the CRC-32 covers the length, the command and the sequence.
    class WAL {
    public:
        /// Opens (or creates) the WAL file in append mode.
        WAL(Settings& set, const std::filesystem::path& walPath);

        /// Empties the file, if something was appended since the last clear (called after a successful flush).
        void clear();

        /// Appends one record. It is buffered: call sync() before acknowledging the write.
        /// @param seq  receives the record's sequence number, if not null.
        /// @return false if the file is not open or the write failed.
        bool append(const std::string& cmd, uint64_t* seq = nullptr);

        /// Pushes the buffered records to the file, with an fsync when `fsyncOnWrite` is set. Several appends can share
        /// one sync (group commit). @return false if the write failed.
        bool sync();

        /// Reads every record of the file up to the first one that is cut by a crash or fails its checksum.
        std::vector<WalEntry> getCmds();

        /// Makes the next sequence numbers start at `next` or above, so new records come after the replayed ones.
        static void advanceSequence(uint64_t next);
        /// The sequence number the next record will get.
        static uint64_t nextSequence();

    private:
        Settings& settings;
        std::fstream wal;
        std::filesystem::path WalPath;
        std::mutex mtx;
        bool canClear;      // something was appended since the last clear
        bool dirty = false; // records were appended since the last sync

        /// Writes the file header to the empty file and syncs it. @return false if the write failed.
        bool writeHeader();
    };

} // namespace Zest
