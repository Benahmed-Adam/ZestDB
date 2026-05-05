#pragma once

#include <chrono>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <vector>

#include "Settings.hpp"

struct WalEntry {
    std::string cmd;
    std::chrono::milliseconds timestamp;
};

class WAL {
public:
    WAL(const Settings& set);
    void clear();
    void append(const std::string& cmd);
    std::vector<WalEntry> getCmds();

private:
    std::fstream wal;
    std::filesystem::path WalPath;
    std::mutex mtx;
    bool canClear;
};