#pragma once

#include <cerrno>
#include <cstring>
#include <ext/stdio_filebuf.h>
#include <fstream>
#include <unistd.h>

#include "Logger.hpp"

namespace Zest {

    inline bool flush_and_fsync(std::fstream& fs) {
        fs.flush();
        if (!fs.good()) {
            ZestLog(LogLevel::ERROR, "flush_and_fsync - flush failed");
            return false;
        }

        struct FdAccess : std::filebuf {
            static int fd(std::filebuf& fb) {
                return (fb.*(&FdAccess::_M_file)).fd();
            }
        };

        std::filebuf* buf = fs.rdbuf();
        if (!buf || !fs.is_open()) {
            ZestLog(LogLevel::ERROR, "Stream is not open or invalid buffer.");
            return false;
        }

        int fd = FdAccess::fd(*buf);
        if (fd == -1) {
            ZestLog(LogLevel::ERROR, "Invalid file descriptor (-1).");
            return false;
        }

        ZestLog(LogLevel::DEBUG, "Flushing and syncing file descriptor: " + std::to_string(fd));
        if (::fdatasync(fd) != 0) {
            ZestLog(LogLevel::ERROR, std::string("fdatasync failed: ") + std::strerror(errno));
            return false;
        }
        return true;
    }
} // namespace Zest
