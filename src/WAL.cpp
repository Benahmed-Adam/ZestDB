#include "WAL.hpp"

#include <atomic>
#include <format>
#include <iostream>

#include "File.hpp"
#include "Logger.hpp"

namespace Zest {

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
    }

    void WAL::clear() {
        std::lock_guard<std::mutex> lock(this->mtx);
        if (this->canClear) {
            this->wal.close();
            this->wal.open(this->WalPath, std::ios::out | std::ios::binary | std::ios::trunc);
            this->wal.close();
            this->wal.open(this->WalPath, std::ios::in | std::ios::out | std::ios::binary | std::ios::app);
            this->canClear = false;
            this->dirty = false;
        }
    }

    bool WAL::append(const std::string& cmd) {
        std::lock_guard<std::mutex> lock(this->mtx);
        if (!this->wal.is_open()) {
            ZestLog(LogLevel::Error, "WAL::append - WAL not open");
            return false;
        }
        this->wal.clear();
        this->canClear = true;

        uint32_t size = static_cast<uint32_t>(cmd.size());
        uint64_t seq = nextSeq.fetch_add(1);

        this->wal.write(reinterpret_cast<const char*>(&size), sizeof(size));
        this->wal.write(cmd.c_str(), static_cast<long int>(size));
        this->wal.write(reinterpret_cast<const char*>(&seq), sizeof(seq));
        this->wal.put('\n');
        this->dirty = true;

        if (!this->wal.good()) {
            ZestLog(LogLevel::Error, "WAL::append - Write failed");
            return false;
        }
        return true;
    }

    // Appends stay in the stream buffer until sync(): callers sync once per batch instead of once per command
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
        uint64_t current = nextSeq.load();
        while (current < next && !nextSeq.compare_exchange_weak(current, next)) {
        }
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
        this->wal.seekg(0, std::ios::beg);

        uint32_t size;
        uint64_t seq;
        while (this->wal.read(reinterpret_cast<char*>(&size), sizeof(size))) {
            if (static_cast<std::streamoff>(size) > fileSize - this->wal.tellg()) {
                ZestLog(LogLevel::Warning, "WAL::getCmds - Truncated record, stopping");
                break;
            }
            std::string cmd(size, '\0');
            if (this->wal.read(&cmd[0], static_cast<long int>(size))) {
                if (this->wal.read(reinterpret_cast<char*>(&seq), sizeof(seq))) {
                    res.push_back({ std::move(cmd), seq });
                } else {
                    break;
                }
            } else {
                break;
            }
            this->wal.peek();
            if (this->wal.peek() == '\n') {
                this->wal.get();
            }
        }

        this->wal.clear();
        this->wal.seekg(0, std::ios::end);
        return res;
    }

} // namespace Zest
