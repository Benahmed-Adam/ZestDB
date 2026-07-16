#include <catch2/catch_all.hpp>
#include "LRUCache.hpp"

using namespace Zest;

static IndexEntry makeEntry(const std::string &key, int segId = 1, unsigned long offset = 0, unsigned int size = 5) {
    IndexEntry e{};
    strncpy(e.key, key.c_str(), sizeof(e.key) - 1);
    e.key[sizeof(e.key) - 1] = '\0';
    e.segmentId = segId;
    e.offset = offset;
    e.size = size;
    e.isTombstone = false;
    return e;
}

TEST_CASE("LRUCache constructor creates empty cache", "[lru]") {
    LRUCache cache(10);
    auto result = cache.get("nonexistent");
    REQUIRE(result.index.segmentId == -1);
    REQUIRE(result.value.empty());
}

TEST_CASE("LRUCache get hit promotes to front", "[lru]") {
    LRUCache cache(10);
    IndexEntry e = makeEntry("key1");
    cache.put(e, "val1");
    auto result = cache.get("key1");
    REQUIRE(result.index.segmentId == 1);
    REQUIRE(result.value == "val1");
}

TEST_CASE("LRUCache get miss returns sentinel", "[lru]") {
    LRUCache cache(10);
    auto result = cache.get("missing");
    REQUIRE(result.index.segmentId == -1);
}

TEST_CASE("LRUCache put new key not full", "[lru]") {
    LRUCache cache(5);
    cache.put(makeEntry("a"), "1");
    cache.put(makeEntry("b"), "2");
    REQUIRE(cache.get("a").value == "1");
    REQUIRE(cache.get("b").value == "2");
}

TEST_CASE("LRUCache put full evicts LRU", "[lru]") {
    LRUCache cache(2);
    cache.put(makeEntry("a"), "1");
    cache.put(makeEntry("b"), "2");
    cache.put(makeEntry("c"), "3");
    auto old = cache.get("a");
    REQUIRE(old.index.segmentId == -1);
    REQUIRE(cache.get("b").value == "2");
    REQUIRE(cache.get("c").value == "3");
}

TEST_CASE("LRUCache put existing key no eviction", "[lru]") {
    LRUCache cache(2);
    cache.put(makeEntry("a"), "1");
    cache.put(makeEntry("a"), "2");
    REQUIRE(cache.get("a").value == "2");
    cache.put(makeEntry("b"), "3");
    REQUIRE(cache.get("a").value == "2");
    REQUIRE(cache.get("b").value == "3");
}

TEST_CASE("LRUCache remove existing key", "[lru]") {
    LRUCache cache(5);
    cache.put(makeEntry("a"), "1");
    cache.remove("a");
    REQUIRE(cache.get("a").index.segmentId == -1);
}

TEST_CASE("LRUCache remove nonexistent key no-op", "[lru]") {
    LRUCache cache(5);
    REQUIRE_NOTHROW(cache.remove("ghost"));
}

TEST_CASE("LRUCache LRU ordering after get/put", "[lru]") {
    LRUCache cache(3);
    cache.put(makeEntry("a"), "1");
    cache.put(makeEntry("b"), "2");
    cache.put(makeEntry("c"), "3");
    cache.get("a");
    cache.put(makeEntry("d"), "4");
    REQUIRE(cache.get("a").value == "1");
    REQUIRE(cache.get("b").index.segmentId == -1);
}

TEST_CASE("LRUCache capacity 1", "[lru]") {
    LRUCache cache(1);
    cache.put(makeEntry("a"), "1");
    REQUIRE(cache.get("a").value == "1");
    cache.put(makeEntry("b"), "2");
    REQUIRE(cache.get("a").index.segmentId == -1);
    REQUIRE(cache.get("b").value == "2");
}
