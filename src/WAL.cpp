#include "WAL.hpp"
#include "Logger.hpp"

WAL::WAL(const Settings& set): canClear(true) {
    ZestLog(LogLevel::INFO, "Opening WAL file...");
    this->walPath = set.walPath;

    this->wal.open(this->walPath, std::ios::in | std::ios::out | std::ios::binary | std::ios::app);
    if (!this->wal.is_open()) {
        this->wal.open(this->walPath, std::ios::out | std::ios::binary);
        this->wal.close();
        this->wal.open(this->walPath, std::ios::in | std::ios::out | std::ios::binary | std::ios::app);
    }
}

void WAL::clear() {
    std::lock_guard<std::mutex> lock(this->mtx);
    if (this->canClear) {
        ZestLog(LogLevel::DEBUG, "WAL::clean - Cleaning the WAL...");
        this->wal.close();
        this->wal.open(this->walPath, std::ios::out | std::ios::binary | std::ios::trunc);
        this->wal.close();
        this->wal.open(this->walPath, std::ios::in | std::ios::out | std::ios::binary | std::ios::app);
        this->canClear = false;
    }
}

void WAL::append(const std::string& cmd) {
    std::lock_guard<std::mutex> lock(this->mtx);
    
    size_t size = cmd.size();
    this->wal.write(reinterpret_cast<const char*>(&size), sizeof(size));
    this->wal.write(cmd.c_str(), static_cast<long int>(size));
    this->wal.flush();
    this->canClear = true;
}

std::vector<std::string> WAL::getCmds() {
    std::lock_guard<std::mutex> lock(this->mtx);
    std::vector<std::string> res;

    this->wal.clear();
    this->wal.seekg(0, std::ios::beg);

    size_t size;
    while (this->wal.read(reinterpret_cast<char*>(&size), sizeof(size))) {
        std::string cmd(size, '\0');
        if (this->wal.read(&cmd[0], static_cast<long int>(size))) {
            res.push_back(std::move(cmd));
        }
    }
    
    this->wal.seekg(0, std::ios::end);
    return res;
}