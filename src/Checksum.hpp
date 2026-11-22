#pragma once

#include <cstdint>
#include <string_view>
#include <zlib.h>

namespace Zest {

    /// CRC-32 of `data`. Pass the previous result as `crc` to continue over several pieces.
    inline uint32_t crc32Of(std::string_view data, uint32_t crc = 0) {
        return static_cast<uint32_t>(::crc32_z(crc, reinterpret_cast<const Bytef*>(data.data()), data.size()));
    }

} // namespace Zest
