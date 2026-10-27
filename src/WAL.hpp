#pragma once

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <vector>

#include "Settings.hpp"

namespace Zest {

    struct WalEntry {
        std::string cmd;
        uint64_t seq;
    };

    class WAL {
    public:
        WAL(Settings& set, const std::filesystem::path& walPath);
        void clear();
        bool append(const std::string& cmd);
        bool sync();
        std::vector<WalEntry> getCmds();
        static void advanceSequence(uint64_t next);

    private:
        Settings& settings;
        std::fstream wal;
        std::filesystem::path WalPath;
        std::mutex mtx;
        bool canClear;
        bool dirty = false;
    };

} // namespace Zest
