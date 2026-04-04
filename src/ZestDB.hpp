#pragma once

#include <memory>

#include "IndexManager.hpp"
#include "Settings.hpp"
#include "LRUCache.hpp"

class ZestDB {
public:
    ZestDB();
    ~ZestDB();

    std::string get(const std::string& key);
    void set(const std::string& key, const std::string& value);
    void del(const std::string key);
private:
    void boot();

    Settings settings;
    IndexManager* indexManager;
    LRUCache cache;
};