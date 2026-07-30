#pragma once

#include <chrono>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <vector>

#include "Settings.hpp"

namespace Zest {

    struct WalEntry {
        std::string cmd;
        std::chrono::milliseconds timestamp;
    };

    class WAL {
    public:
        WAL(Settings &set, const std::filesystem::path &walPath);
        void clear();
        void append(const std::string &cmd);
        std::vector<WalEntry> getCmds();

    private:
        Settings &settings;
        std::fstream wal;
        std::filesystem::path WalPath;
        std::mutex mtx;
        bool canClear;
    };

} // namespace Zest
