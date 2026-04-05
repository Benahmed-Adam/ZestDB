#pragma once

#include <filesystem>

struct Settings {
    std::filesystem::path DbPath;
    std::filesystem::path IndexPath;
    unsigned long SegSize;
    unsigned int MaxKeySize;
    unsigned int MaxValueSize;
};