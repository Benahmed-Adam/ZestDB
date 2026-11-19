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
    /// index and segments are flushed, the WAL is cleared. Record format: `uint32 length`, the command,
    /// `uint64 sequence`, `'\n'`.
    class WAL {
    public:
        /// Opens (or creates) the WAL file in append mode.
        WAL(Settings& set, const std::filesystem::path& walPath);

        /// Empties the file, if something was appended since the last clear (called after a successful flush).
        void clear();

        /// Appends one record. It is buffered: call sync() before acknowledging the write.
        /// @return false if the file is not open or the write failed.
        bool append(const std::string& cmd);

        /// Pushes the buffered records to the file, with an fsync when `fsyncOnWrite` is set. Several appends can share
        /// one sync (group commit). @return false if the write failed.
        bool sync();

        /// Reads every complete record of the file; a record cut by a crash ends the read.
        std::vector<WalEntry> getCmds();

        /// Makes the next sequence numbers start at `next` or above, so new records come after the replayed ones.
        static void advanceSequence(uint64_t next);

    private:
        Settings& settings;
        std::fstream wal;
        std::filesystem::path WalPath;
        std::mutex mtx;
        bool canClear;      // something was appended since the last clear
        bool dirty = false; // records were appended since the last sync
    };

} // namespace Zest
