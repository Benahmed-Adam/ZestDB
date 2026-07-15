#include "LRUCache.hpp"
#include "Logger.hpp"
#include <format>

namespace Zest {

LRUCache::LRUCache(unsigned int cap)
    : capacity(cap)
{
    ZestLog(LogLevel::DEBUG, std::format("LRUCache::LRUCache - created cache with capacity: {}", cap));
}

CacheEntry LRUCache::get(const std::string& key)
{
    ZestLog(LogLevel::DEBUG, std::format("LRUCache::get - looking for key: {}", key));

    std::lock_guard<std::mutex> lock(this->mtx);

    auto it = this->map.find(key);
    if (it == this->map.end()) {
        ZestLog(LogLevel::DEBUG, "LRUCache::get - key not in cache");
        return { { "", -1, 0, 0, false }, "" };
    }

    this->lru_list.erase(it->second.second);
    this->lru_list.push_front(key);

    it->second.second = this->lru_list.begin();

    ZestLog(LogLevel::DEBUG, "LRUCache::get - key found in cache");
    return it->second.first;
}

void LRUCache::put(const IndexEntry& entry, const std::string& value)
{
    std::string key(entry.key);

    ZestLog(LogLevel::DEBUG, std::format("LRUCache::put - putting key: {} with value: {}", key, value));

    std::lock_guard<std::mutex> lock(this->mtx);

    auto it = this->map.find(key);

    if (it != this->map.end()) {
        this->lru_list.erase(it->second.second);
    } else {
        if (this->map.size() >= this->capacity) {
            ZestLog(LogLevel::DEBUG, "LRUCache::put - cache full, evicting LRU item");
            std::string last = this->lru_list.back();
            this->lru_list.pop_back();
            this->map.erase(last);
        }
    }

    this->lru_list.push_front(key);
    this->map[key] = { { entry, value }, this->lru_list.begin() };
    ZestLog(LogLevel::DEBUG, std::format("LRUCache::put - key inserted, cache size: {}", this->map.size()));
}

void LRUCache::remove(const std::string& key)
{
    ZestLog(LogLevel::DEBUG, std::format("LRUCache::remove - removing key: {}", key));

    std::lock_guard<std::mutex> lock(this->mtx);

    auto it = this->map.find(key);

    if (it != this->map.end()) {
        this->lru_list.erase(it->second.second);
        this->map.erase(it);
        ZestLog(LogLevel::DEBUG, "LRUCache::remove - key removed");
    }
}

} // namespace Zest
