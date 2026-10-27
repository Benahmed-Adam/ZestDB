#pragma once

#include <cerrno>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>

#ifdef _WIN32
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

#include "Logger.hpp"

namespace Zest {

    inline bool sync_to_disk(const std::filesystem::path& path) {
#ifdef _WIN32
        HANDLE handle = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (handle == INVALID_HANDLE_VALUE) {
            ZestLog(LogLevel::Error, std::format("sync_to_disk - cannot open {} (error {})", path.string(), GetLastError()));
            return false;
        }
        const bool ok = FlushFileBuffers(handle) != 0;
        CloseHandle(handle);
#else
        const int fd = ::open(path.c_str(), O_RDONLY);
        if (fd == -1) {
            ZestLog(LogLevel::Error, std::format("sync_to_disk - cannot open {}: {}", path.string(), std::strerror(errno)));
            return false;
        }
#ifdef __APPLE__
        const bool ok = ::fsync(fd) == 0;
#else
        const bool ok = ::fdatasync(fd) == 0;
#endif
        ::close(fd);
#endif
        if (!ok) {
            ZestLog(LogLevel::Error, std::format("sync_to_disk - sync failed for {}", path.string()));
        }
        return ok;
    }

    inline bool flush_and_fsync(std::fstream& fs, const std::filesystem::path& path) {
        if (!fs.is_open()) {
            ZestLog(LogLevel::Error, "flush_and_fsync - stream is not open");
            return false;
        }
        fs.flush();
        if (!fs.good()) {
            ZestLog(LogLevel::Error, "flush_and_fsync - flush failed");
            return false;
        }
        return sync_to_disk(path);
    }
} // namespace Zest
