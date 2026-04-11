#pragma once

#include <filesystem>
#include <regex>
#include <string>

struct Settings {
    std::filesystem::path DbPath;
    std::filesystem::path IndexPath;
    unsigned long SegSize;
    unsigned int MaxKeySize;
    unsigned int MaxValueSize;
    unsigned int CacheSize;
    std::regex KeyValidation;
    std::regex ValueValidation;
    std::regex NetworkValidation;
    std::string KeyValidationStr;
    std::string ValueValidationStr;
    std::string NetworkValidationStr;

    bool isDebug;
};