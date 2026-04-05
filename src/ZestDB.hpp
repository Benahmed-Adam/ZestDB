#pragma once

#include <memory>

#include "IndexManager.hpp"
#include "LRUCache.hpp"
#include "Settings.hpp"
#include "StorageManager.hpp"

enum class ResultType {
    SUCCESS,
    ERROR
};

class ZestDB {
public:
    ZestDB();
    ~ZestDB();

    std::string get(const std::string& key);
    ResultType set(const std::string& key, const std::string& value);
    ResultType del(const std::string key);

private:
    void boot();

    Settings settings;
    IndexManager* indexManager;
    StorageManager* storageManager;
    LRUCache cache;
};