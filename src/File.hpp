#include <ext/stdio_filebuf.h>
#include <fstream>

#include "Logger.hpp"

namespace Zest {

    inline void flush_and_fsync(std::fstream &fs) {
        fs.flush();

        struct GnuFileBuf : std::filebuf {
            int get_fd() { return this->_M_file.fd(); }
        };

        auto *buf = static_cast<GnuFileBuf *>(fs.rdbuf());
        if (buf && fs.is_open()) {
            int fd = buf->get_fd();
            if (fd != -1) {
                ZestLog(LogLevel::DEBUG, "Flushing and syncing file descriptor: " + std::to_string(fd));
                ::fdatasync(fd);
            } else {
                ZestLog(LogLevel::ERROR, "Invalid file descriptor (-1).");
            }
        } else {
            ZestLog(LogLevel::ERROR, "Stream is not open or invalid buffer.");
        }
    }
} // namespace Zest