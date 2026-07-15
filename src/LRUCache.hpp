#pragma once

#include <list>
#include <mutex>
#include <string>
#include <unordered_map>

#include "IndexManager.hpp"

namespace Zest {

    struct CacheEntry {
        IndexEntry index;
        std::string value;
    };

    class LRUCache {
    public:
        LRUCache(unsigned int cap);

        CacheEntry get(const std::string &key);
        void put(const IndexEntry &entry, const std::string &value);
        void remove(const std::string &key);

    private:
        unsigned int capacity;

        std::list<std::string> lru_list;

        using MapIter = std::list<std::string>::iterator;
        std::unordered_map<std::string, std::pair<CacheEntry, MapIter>> map;
        std::mutex mtx;
    };

} // namespace Zest
