#include "WAL.hpp"

#include <atomic>
#include <format>
#include <iostream>
#include <string_view>

#include "Checksum.hpp"
#include "File.hpp"
#include "Logger.hpp"

namespace Zest {

    namespace {
        constexpr std::string_view WAL_MAGIC("ZWAL\x01", 5); // file header: format name and version

        uint32_t recordCrc(uint32_t size, std::string_view cmd, uint64_t seq) {
            uint32_t crc = crc32Of(std::string_view(reinterpret_cast<const char*>(&size), sizeof(size)));
            crc = crc32Of(cmd, crc);
            return crc32Of(std::string_view(reinterpret_cast<const char*>(&seq), sizeof(seq)), crc);
        }
    } // namespace

    // Shared by every shard: the sequence numbers order the records of all the WALs, so the replay can merge them
    static std::atomic<uint64_t> nextSeq{ 0 };

    WAL::WAL(Settings& set, const std::filesystem::path& walPath)
        : settings(set),
          canClear(true) {
        ZestLog(LogLevel::Info, "Opening WAL file...");
        this->WalPath = walPath;

        this->wal.open(this->WalPath, std::ios::in | std::ios::out | std::ios::binary | std::ios::app);
        if (!this->wal.is_open()) {
            this->wal.open(this->WalPath, std::ios::out | std::ios::binary);
            this->wal.close();
            this->wal.open(this->WalPath, std::ios::in | std::ios::out | std::ios::binary | std::ios::app);
        }

        std::string head(WAL_MAGIC.size(), '\0');
        this->wal.seekg(0, std::ios::beg);
        this->wal.read(head.data(), static_cast<std::streamsize>(head.size()));
        const std::streamsize headSize = this->wal.gcount();
        this->wal.clear();
        if (headSize == 0) {
            this->writeHeader();
        } else if (head != WAL_MAGIC) {
            ZestLog(LogLevel::Warning, std::format("WAL - {} has a damaged file header, reading its records anyway", this->WalPath.string()));
        }
    }

    bool WAL::writeHeader() {
        this->wal.write(WAL_MAGIC.data(), static_cast<std::streamsize>(WAL_MAGIC.size()));
        // Synced right away: a header cut by a crash would make the records after it unreadable
        if (!(this->settings.fsyncOnWrite ? flush_and_fsync(this->wal, this->WalPath) : this->wal.flush().good())) {
            ZestLog(LogLevel::Error, "WAL - cannot write the file header");
            return false;
        }
        return true;
    }

    void WAL::clear() {
        std::lock_guard<std::mutex> lock(this->mtx);
        if (this->canClear) {
            this->wal.close();
            this->wal.open(this->WalPath, std::ios::out | std::ios::binary | std::ios::trunc);
            this->wal.close();
            this->wal.open(this->WalPath, std::ios::in | std::ios::out | std::ios::binary | std::ios::app);
            this->writeHeader();
            this->canClear = false;
            this->dirty = false;
        }
    }

    bool WAL::append(const std::string& cmd, uint64_t* seqOut) {
        std::lock_guard<std::mutex> lock(this->mtx);
        if (!this->wal.is_open()) {
            ZestLog(LogLevel::Error, "WAL::append - WAL not open");
            return false;
        }
        this->wal.clear();
        this->canClear = true;

        uint32_t size = static_cast<uint32_t>(cmd.size());
        uint64_t seq = nextSeq.fetch_add(1);
        if (seqOut) {
            *seqOut = seq;
        }

        const uint32_t crc = recordCrc(size, cmd, seq);
        this->wal.write(reinterpret_cast<const char*>(&size), sizeof(size));
        this->wal.write(reinterpret_cast<const char*>(&crc), sizeof(crc));
        this->wal.write(cmd.c_str(), static_cast<long int>(size));
        this->wal.write(reinterpret_cast<const char*>(&seq), sizeof(seq));
        this->dirty = true;

        if (!this->wal.good()) {
            ZestLog(LogLevel::Error, "WAL::append - Write failed");
            return false;
        }
        return true;
    }

    bool WAL::sync() {
        std::lock_guard<std::mutex> lock(this->mtx);
        if (!this->dirty) {
            return true;
        }
        bool ok = this->settings.fsyncOnWrite ? flush_and_fsync(this->wal, this->WalPath) : this->wal.flush().good();
        if (!ok) {
            ZestLog(LogLevel::Error, "WAL::sync - Write failed");
            return false;
        }
        this->dirty = false;
        return true;
    }

    void WAL::advanceSequence(uint64_t next) {
        // Atomic max: never move the counter backwards, even if another thread advanced it in the meantime
        uint64_t current = nextSeq.load();
        while (current < next && !nextSeq.compare_exchange_weak(current, next)) {
        }
    }

    uint64_t WAL::nextSequence() {
        return nextSeq.load();
    }

    std::vector<WalEntry> WAL::getCmds() {
        std::lock_guard<std::mutex> lock(this->mtx);
        std::vector<WalEntry> res;

        if (!this->wal.is_open()) {
            ZestLog(LogLevel::Error, "WAL::getCmds - WAL not open");
            return res;
        }

        this->wal.clear();
        this->wal.seekg(0, std::ios::end);
        const std::streamoff fileSize = this->wal.tellg();

        this->wal.seekg(static_cast<std::streamoff>(WAL_MAGIC.size()), std::ios::beg);
        uint32_t size;
        uint32_t crc;
        uint64_t seq;
        while (this->wal.read(reinterpret_cast<char*>(&size), sizeof(size)) && this->wal.read(reinterpret_cast<char*>(&crc), sizeof(crc))) {
            // A length past the end of the file is a record cut by a crash (or garbage): do not allocate it
            if (static_cast<std::streamoff>(size) > fileSize - this->wal.tellg()) {
                ZestLog(LogLevel::Warning, "WAL::getCmds - Truncated record, stopping");
                break;
            }
            std::string cmd(size, '\0');
            if (!this->wal.read(cmd.data(), static_cast<std::streamsize>(size)) || !this->wal.read(reinterpret_cast<char*>(&seq), sizeof(seq))) {
                break;
            }
            // The records after a damaged one may depend on it: the replay stops there instead of skipping it
            if (recordCrc(size, cmd, seq) != crc) {
                ZestLog(LogLevel::Error, std::format("WAL::getCmds - Record {} of {} fails its checksum, stopping the replay of this WAL there", res.size() + 1, this->WalPath.string()));
                break;
            }
            res.push_back({ std::move(cmd), seq });
        }

        this->wal.clear();
        this->wal.seekg(0, std::ios::end);
        return res;
    }

} // namespace Zest
