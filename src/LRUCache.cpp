#include "LRUCache.hpp"

namespace Zest {

    LRUCache::LRUCache(unsigned int cap)
        : capacity(cap) {}

    CacheEntry LRUCache::get(const std::string &key) {
        std::lock_guard<std::mutex> lock(this->mtx);

        auto it = this->map.find(key);
        if (it == this->map.end()) {
            return { { "", -1, 0, 0, false }, "" };
        }

        this->lru_list.erase(it->second.second);
        this->lru_list.push_front(key);

        it->second.second = this->lru_list.begin();

        return it->second.first;
    }

    void LRUCache::put(const IndexEntry &entry, const std::string &value) {
        std::string key(entry.key);

        std::lock_guard<std::mutex> lock(this->mtx);

        auto it = this->map.find(key);

        if (it != this->map.end()) {
            this->lru_list.erase(it->second.second);
        } else {
            if (this->map.size() >= this->capacity) {
                std::string last = this->lru_list.back();
                this->lru_list.pop_back();
                this->map.erase(last);
            }
        }

        this->lru_list.push_front(key);
        this->map[key] = { { entry, value }, this->lru_list.begin() };
    }

    void LRUCache::remove(const std::string &key) {
        std::lock_guard<std::mutex> lock(this->mtx);

        auto it = this->map.find(key);

        if (it != this->map.end()) {
            this->lru_list.erase(it->second.second);
            this->map.erase(it);
        }
    }

} // namespace Zest
