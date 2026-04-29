#include "Utils.hpp"

#ifdef _WIN32
#include <windows.h>
#include <io.h>
#else
#include <unistd.h>
#endif


void ZestFsync(int fd) {
#ifdef _WIN32
    HANDLE h = (HANDLE)_get_osfhandle(fd);
    if (h != INVALID_HANDLE_VALUE) {
        FlushFileBuffers(h);
    }
#else
    fsync(fd);
#endif
}