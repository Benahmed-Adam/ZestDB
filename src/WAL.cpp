#include "WAL.hpp"
#include "Logger.hpp"
#include <algorithm>
#include <format>
#include <iostream>

WAL::WAL(const Settings& set)
    : canClear(true)
{
    ZestLog(LogLevel::INFO, "Opening WAL file...");
    this->WalPath = set.WalPath;

    this->wal.open(this->WalPath, std::ios::in | std::ios::out | std::ios::binary | std::ios::app);
    if (!this->wal.is_open()) {
        this->wal.open(this->WalPath, std::ios::out | std::ios::binary);
        this->wal.close();
        this->wal.open(this->WalPath, std::ios::in | std::ios::out | std::ios::binary | std::ios::app);
    }
}

void WAL::clear()
{
    std::lock_guard<std::mutex> lock(this->mtx);
    if (this->canClear) {
        ZestLog(LogLevel::DEBUG, "WAL::clean - Cleaning the WAL...");
        this->wal.close();
        this->wal.open(this->WalPath, std::ios::out | std::ios::binary | std::ios::trunc);
        this->wal.close();
        this->wal.open(this->WalPath, std::ios::in | std::ios::out | std::ios::binary | std::ios::app);
        this->canClear = false;
    }
}

void WAL::append(const std::string& cmd)
{
    std::lock_guard<std::mutex> lock(this->mtx);
    if (!this->wal.is_open()) {
        ZestLog(LogLevel::ERROR, "WAL::append - WAL not open");
        return;
    }
    size_t size = cmd.size();
    auto timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::high_resolution_clock::now().time_since_epoch());

    this->wal.write(reinterpret_cast<const char*>(&size), sizeof(size));
    this->wal.write(cmd.c_str(), static_cast<long int>(size));
    this->wal.write(reinterpret_cast<const char*>(&timestamp), sizeof(timestamp));
    this->wal.put('\n');
    this->wal.flush();
    // fsync ?
    if (!this->wal.good()) {
        ZestLog(LogLevel::ERROR, "WAL::append - Write failed");
    }
    this->canClear = true;
}

std::vector<WalEntry> WAL::getCmds()
{
    std::lock_guard<std::mutex> lock(this->mtx);
    std::vector<WalEntry> res;

    if (!this->wal.is_open()) {
        ZestLog(LogLevel::ERROR, "WAL::getCmds - WAL not open");
        return res;
    }

    this->wal.clear();
    this->wal.seekg(0, std::ios::beg);

    size_t size;
    std::chrono::milliseconds timestamp;
    while (this->wal.read(reinterpret_cast<char*>(&size), sizeof(size))) {
        if (size > 1000000) {
            ZestLog(LogLevel::WARNING, "WAL::getCmds - Invalid size, stopping");
            break;
        }
        std::string cmd(size, '\0');
        if (this->wal.read(&cmd[0], static_cast<long int>(size))) {
            if (this->wal.read(reinterpret_cast<char*>(&timestamp), sizeof(timestamp))) {
                res.push_back({ std::move(cmd), timestamp });
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

    std::sort(res.begin(), res.end(), [](const WalEntry& a, const WalEntry& b) {
        return a.timestamp < b.timestamp;
    });

    this->wal.clear();
    this->wal.seekg(0, std::ios::end);
    return res;
}