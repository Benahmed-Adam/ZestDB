#include "LRUCache.hpp"
#include "Logger.hpp"

LRUCache::LRUCache(unsigned int cap)
    : capacity(cap)
{
    ZestLog(LogLevel::DEBUG, "LRUCache::LRUCache - created cache with capacity: " + std::to_string(cap));
}

CacheEntry LRUCache::get(const std::string& key)
{
    ZestLog(LogLevel::DEBUG, "LRUCache::get - looking for key: " + key);

    std::lock_guard<std::mutex> lock(this->mtx);

    auto it = map.find(key);
    if (it == map.end()) {
        ZestLog(LogLevel::DEBUG, "LRUCache::get - key not in cache");
        return { { "", -1, 0, 0, false }, "" };
    }

    lru_list.erase(it->second.second);
    lru_list.push_front(key);

    it->second.second = lru_list.begin();

    ZestLog(LogLevel::DEBUG, "LRUCache::get - key found in cache");
    return it->second.first;
}

void LRUCache::put(const IndexEntry& entry, const std::string& value)
{
    std::string key(entry.key);

    ZestLog(LogLevel::DEBUG, "LRUCache::put - putting key: " + key);

    std::lock_guard<std::mutex> lock(this->mtx);

    auto it = map.find(key);

    if (it != map.end()) {
        lru_list.erase(it->second.second);
    } else {
        if (map.size() >= capacity) {
            ZestLog(LogLevel::DEBUG, "LRUCache::put - cache full, evicting LRU item");
            std::string last = lru_list.back();
            lru_list.pop_back();
            map.erase(last);
        }
    }

    lru_list.push_front(key);
    map[key] = { { entry, value }, lru_list.begin() };
    ZestLog(LogLevel::DEBUG, "LRUCache::put - key inserted, cache size: " + std::to_string(map.size()));
}

void LRUCache::remove(const std::string& key)
{
    ZestLog(LogLevel::DEBUG, "LRUCache::remove - removing key: " + key);

    std::lock_guard<std::mutex> lock(this->mtx);

    auto it = map.find(key);

    if (it != map.end()) {
        lru_list.erase(it->second.second);
        map.erase(it);
        ZestLog(LogLevel::DEBUG, "LRUCache::remove - key removed");
    }
}