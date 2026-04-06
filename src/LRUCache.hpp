#pragma once

#include "IndexManager.hpp"
#include <list>
#include <string>
#include <unordered_map>

class LRUCache {
public:
    LRUCache(unsigned int cap);

    IndexEntry get(const std::string& key);
    void put(const IndexEntry& entry);
    void remove(const std::string& key);

private:
    unsigned int capacity;

    std::list<std::string> lru_list;

    using MapIter = std::list<std::string>::iterator;
    std::unordered_map<std::string, std::pair<IndexEntry, MapIter>> map;
};