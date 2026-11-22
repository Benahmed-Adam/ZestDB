#pragma once

#include <list>
#include <mutex>
#include <string>
#include <unordered_map>

#include "IndexManager.hpp"

namespace Zest {

    /// A cached value with the index entry it was read from.
    struct CacheEntry {
        IndexEntry index;
        std::string value;
    };

    /// Least-recently-used cache of values, one per shard, holding at most `capacity` keys.
    /// Reads fill it; writes only refresh keys already cached, so a burst of writes does not evict the hot keys.
    class LRUCache {
    public:
        /// A capacity of 0 disables the cache.
        LRUCache(unsigned int cap);

        /// The cached entry for `key` (which becomes the most recently used), or an entry with `INVALID_SEGMENT_ID` on a miss.
        CacheEntry get(const std::string& key);

        /// Inserts or replaces `entry.key`, evicting the least recently used key when the cache is full.
        void put(const IndexEntry& entry, const std::string& value);

        /// Replaces `entry.key` only if it is already cached; never inserts.
        void update(const IndexEntry& entry, const std::string& value);

        /// Removes `key` if it is cached.
        void remove(const std::string& key);

        /// Changes the capacity, evicting the least recently used keys if it shrinks.
        void resize(unsigned int cap);

    private:
        unsigned int capacity;

        std::list<std::string> lru_list; // most recently used first

        using MapIter = std::list<std::string>::iterator;
        std::unordered_map<std::string, std::pair<CacheEntry, MapIter>> map; // key -> entry and its node in lru_list
        std::mutex mtx;
    };

} // namespace Zest
