#include "LRUCache.hpp"

LRUCache::LRUCache(unsigned int cap) : capacity(cap) {}

IndexEntry LRUCache::get(const std::string& key) {
    auto it = map.find(key);
    if (it == map.end()) {
        return IndexEntry();
    }

    lru_list.erase(it->second.second);
    lru_list.push_front(key);
    
    it->second.second = lru_list.begin();

    return it->second.first;
}

void LRUCache::put(const std::string& key, const IndexEntry& entry) {
    auto it = map.find(key);

    if (it != map.end()) {
        lru_list.erase(it->second.second);
    } else {
        if (map.size() >= capacity) {
            std::string last = lru_list.back();
            lru_list.pop_back();
            map.erase(last);
        }
    }

    lru_list.push_front(key);
    map[key] = {entry, lru_list.begin()};
}

void LRUCache::remove(const std::string& key) {
    auto it = map.find(key);
    if (it != map.end()) {
        lru_list.erase(it->second.second);
        map.erase(it);
    }
}