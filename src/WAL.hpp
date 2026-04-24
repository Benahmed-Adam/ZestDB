#pragma once

#include <filesystem>
#include <fstream>
#include <mutex>

#include "Settings.hpp"

class WAL {
public:
    WAL(const Settings& set);
    void clear();
    void append(const std::string& cmd);
    std::vector<std::string> getCmds();

private:
    std::fstream wal;
    std::filesystem::path WalPath;
    std::mutex mtx;
    bool canClear;
};