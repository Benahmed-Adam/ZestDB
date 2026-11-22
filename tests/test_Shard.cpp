#include <atomic>
#include <catch2/catch_all.hpp>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <thread>

#include "Shard.hpp"

using namespace Zest;
namespace fs = std::filesystem;

class ShardFixture {
public:
    Settings settings;
    fs::path testDir;

    ShardFixture() {
        testDir = fs::temp_directory_path() / ("zestdb_test_shard_" + std::to_string(reinterpret_cast<uintptr_t>(this)));
        fs::create_directories(testDir);
        settings.DbPath = testDir;
        settings.SegSize = 100000;
        settings.MaxKeySize = 255;
        settings.MaxValueSize = 10000;
        settings.CacheSize = 100;
        settings.CompactingInterval = 3600;
        settings.FlushInterval = 3600;
        settings.isDebug = false;
    }

    ~ShardFixture() {
        fs::remove_all(testDir);
    }
};

TEST_CASE_METHOD(ShardFixture, "Shard set and get", "[shard]") {
    Shard shard(settings, 0);
    auto setResult = shard.set("key1", "value1");
    REQUIRE(setResult.code == ResultType::Code::Success);
    auto getResult = shard.get("key1");
    REQUIRE(getResult.code == ResultType::Code::Success);
    REQUIRE(getResult.response == "value1");
}

TEST_CASE_METHOD(ShardFixture, "Shard get key not found", "[shard]") {
    Shard shard(settings, 0);
    auto result = shard.get("nonexistent");
    REQUIRE(result.code == ResultType::Code::KeyNotFound);
}

TEST_CASE_METHOD(ShardFixture, "Shard del existing key", "[shard]") {
    Shard shard(settings, 0);
    shard.set("key1", "value1");
    auto delResult = shard.del("key1");
    REQUIRE(delResult.code == ResultType::Code::Success);
    auto getResult = shard.get("key1");
    REQUIRE(getResult.code == ResultType::Code::KeyNotFound);
}

TEST_CASE_METHOD(ShardFixture, "Shard del key not found", "[shard]") {
    Shard shard(settings, 0);
    auto result = shard.del("ghost");
    REQUIRE(result.code == ResultType::Code::KeyNotFound);
}

TEST_CASE_METHOD(ShardFixture, "Shard set overwrite same key", "[shard]") {
    Shard shard(settings, 0);
    shard.set("key1", "val1");
    shard.set("key1", "val2");
    auto result = shard.get("key1");
    REQUIRE(result.response == "val2");
}

TEST_CASE_METHOD(ShardFixture, "Shard getBy with pattern", "[shard]") {
    Shard shard(settings, 0);
    shard.set("abc_1", "v1");
    shard.set("abc_2", "v2");
    shard.set("xyz_1", "v3");
    ValidationRule vr;
    vr.func = [](std::string_view k) { return k.find("abc_") == 0; };
    vr.limit = UINT_MAX;
    auto result = shard.getBy(vr);
    REQUIRE(result.code == ResultType::Code::Success);
    REQUIRE(result.affectedRows == 2);
}

TEST_CASE_METHOD(ShardFixture, "Shard getBy with limit", "[shard]") {
    Shard shard(settings, 0);
    shard.set("a1", "v1");
    shard.set("a2", "v2");
    shard.set("a3", "v3");
    ValidationRule vr;
    std::atomic<unsigned int> counter(0);
    vr.func = [](std::string_view) { return true; };
    vr.limit = 2;
    vr.globalMatchCount = &counter;
    auto result = shard.getBy(vr);
    REQUIRE(result.affectedRows == 2);
}

TEST_CASE_METHOD(ShardFixture, "Shard setBy updates matching keys", "[shard]") {
    Shard shard(settings, 0);
    shard.set("abc_1", "old");
    shard.set("abc_2", "old");
    shard.set("xyz_1", "old");
    ValidationRule vr;
    vr.func = [](std::string_view k) { return k.find("abc_") == 0; };
    vr.limit = UINT_MAX;
    auto result = shard.setBy(vr, "new");
    REQUIRE(result.code == ResultType::Code::Success);
    REQUIRE(result.affectedRows == 2);
    REQUIRE(shard.get("abc_1").response == "new");
}

TEST_CASE_METHOD(ShardFixture, "Shard delBy tombstones matching keys", "[shard]") {
    Shard shard(settings, 0);
    shard.set("abc_1", "v1");
    shard.set("abc_2", "v2");
    shard.set("xyz_1", "v3");
    ValidationRule vr;
    vr.func = [](std::string_view k) { return k.find("abc_") == 0; };
    vr.limit = UINT_MAX;
    auto result = shard.delBy(vr);
    REQUIRE(result.code == ResultType::Code::Success);
    REQUIRE(result.affectedRows == 2);
    REQUIRE(shard.get("abc_1").code == ResultType::Code::KeyNotFound);
    REQUIRE(shard.get("xyz_1").code == ResultType::Code::Success);
}

TEST_CASE_METHOD(ShardFixture, "Shard flush", "[shard]") {
    Shard shard(settings, 0);
    shard.set("key1", "value1");
    REQUIRE_NOTHROW(shard.flush());
}

TEST_CASE_METHOD(ShardFixture, "Shard stop single execution", "[shard]") {
    Shard shard(settings, 0);
    REQUIRE_NOTHROW(shard.stop());
    REQUIRE_NOTHROW(shard.stop());
}

TEST_CASE_METHOD(ShardFixture, "Shard multiple operations", "[shard]") {
    Shard shard(settings, 0);
    for (int i = 0; i < 50; i++) {
        shard.set("key_" + std::to_string(i), "val_" + std::to_string(i));
    }
    for (int i = 0; i < 50; i++) {
        auto r = shard.get("key_" + std::to_string(i));
        REQUIRE(r.code == ResultType::Code::Success);
        REQUIRE(r.response == "val_" + std::to_string(i));
    }
}

TEST_CASE_METHOD(ShardFixture, "Shard WAL append works", "[shard]") {
    Shard shard(settings, 0);
    auto& wal = shard.getWAL();
    wal.append("set key1 val1");
    auto cmds = wal.getCmds();
    REQUIRE_FALSE(cmds.empty());
    REQUIRE(cmds[0].cmd == "set key1 val1");
}

TEST_CASE_METHOD(ShardFixture, "Shard concurrent writes, flushes and compactions keep every value", "[shard]") {
    constexpr int writers = 4;
    constexpr int perWriter = 200;

    auto expected = [](int w, int i) -> std::string { return i % 3 == 0 ? "" : std::format("k{}_{}_v2", w, i); };
    auto checkAll = [&](Shard& shard) {
        for (int w = 0; w < writers; w++) {
            for (int i = 0; i < perWriter; i++) {
                auto r = shard.get(std::format("k{}_{}", w, i));
                if (expected(w, i).empty()) {
                    REQUIRE(r.code == ResultType::Code::KeyNotFound);
                } else {
                    REQUIRE(r.response == expected(w, i));
                }
            }
        }
    };

    {
        Shard shard(settings, 0);
        std::atomic<bool> done = false;
        std::thread flusher([&] {
            do {
                shard.flush();
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            } while (!done);
        });
        std::thread compactor([&] {
            do {
                shard.compact();
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            } while (!done);
        });
        std::vector<std::thread> threads;
        for (int w = 0; w < writers; w++) {
            threads.emplace_back([&shard, w] {
                for (int i = 0; i < perWriter; i++) {
                    std::string key = std::format("k{}_{}", w, i);
                    shard.set(key, key + "_v1");
                    shard.set(key, key + "_v2");
                    if (i % 3 == 0) {
                        shard.del(key);
                    }
                    shard.get(key);
                }
            });
        }
        for (auto& t : threads) {
            t.join();
        }
        done = true;
        flusher.join();
        compactor.join();
        checkAll(shard);
    }

    Shard reopened(settings, 0);
    checkAll(reopened);
}

TEST_CASE_METHOD(ShardFixture, "Shard concurrent increments never lose an update", "[shard][incr]") {
    constexpr int threads = 8;
    constexpr int perThread = 500;
    Shard shard(settings, 0);
    std::vector<std::thread> workers;
    for (int t = 0; t < threads; t++) {
        workers.emplace_back([&shard] {
            uint64_t expiresAt = 0;
            for (int i = 0; i < perThread; i++) {
                shard.incr("counter", 1, expiresAt);
            }
        });
    }
    for (auto& worker : workers) {
        worker.join();
    }
    REQUIRE(shard.get("counter").response == std::to_string(threads * perThread));
}

TEST_CASE_METHOD(ShardFixture, "Shard concurrent setnx on one key lets exactly one writer win", "[shard][setnx]") {
    constexpr int threads = 8;
    constexpr int rounds = 50;
    Shard shard(settings, 0);
    for (int round = 0; round < rounds; round++) {
        const std::string key = std::format("lock{}", round);
        std::atomic<int> winners = 0;
        std::vector<std::thread> workers;
        for (int t = 0; t < threads; t++) {
            workers.emplace_back([&, t] {
                if (shard.setnx(key, std::format("owner{}", t)).code == ResultType::Code::Success) {
                    winners++;
                }
            });
        }
        for (auto& worker : workers) {
            worker.join();
        }
        REQUIRE(winners == 1);
    }
}

TEST_CASE_METHOD(ShardFixture, "Shard concurrent cas retries never lose an update", "[shard][cas]") {
    constexpr int threads = 8;
    constexpr int perThread = 100;
    Shard shard(settings, 0);
    shard.set("counter", "0");
    std::vector<std::thread> workers;
    for (int t = 0; t < threads; t++) {
        workers.emplace_back([&shard] {
            uint64_t expiresAt = 0;
            for (int i = 0; i < perThread; i++) {
                while (true) {
                    const std::string current = shard.get("counter").response;
                    if (shard.cas("counter", current, std::to_string(std::stoi(current) + 1), expiresAt).code == ResultType::Code::Success) {
                        break;
                    }
                }
            }
        });
    }
    for (auto& worker : workers) {
        worker.join();
    }
    REQUIRE(shard.get("counter").response == std::to_string(threads * perThread));
}

TEST_CASE_METHOD(ShardFixture, "Shard exists finds keys that are not in the cache", "[shard][exists]") {
    settings.CacheSize = 10;
    Shard shard(settings, 0);
    for (int i = 0; i < 100; i++) {
        shard.set(std::format("k{}", i), "v");
    }
    for (int i = 0; i < 100; i++) {
        REQUIRE(shard.exists(std::format("k{}", i)).response == "true");
    }
    shard.del("k5");
    REQUIRE(shard.exists("k5").response == "false");
    REQUIRE(shard.exists("absent").response == "false");
}

TEST_CASE_METHOD(ShardFixture, "Shard writes do not fill the cache, reads do, and cached values stay fresh", "[shard][lru]") {
    Shard shard(settings, 0);
    auto misses = [&shard] { return shard.getPerfMonitoring().getPerformances()["get"]["nbCacheMisses"].get<unsigned int>(); };

    shard.set("k", "v1");
    REQUIRE(shard.get("k").response == "v1");
    REQUIRE(misses() == 1);
    REQUIRE(shard.get("k").response == "v1");
    REQUIRE(misses() == 1);

    shard.set("k", "v2");
    REQUIRE(shard.get("k").response == "v2");
    REQUIRE(misses() == 1);
    uint64_t expiresAt = 0;
    shard.incr("n", 1, expiresAt);
    REQUIRE(shard.get("n").response == "1");
    REQUIRE(misses() == 2);
}

TEST_CASE_METHOD(ShardFixture, "Shard reloadSettings resizes the cache, 0 disables it", "[shard]") {
    Shard shard(settings, 0);
    shard.set("key1", "value1");
    Settings s = settings;
    s.CacheSize = 0;
    shard.reloadSettings(s);
    shard.set("key2", "value2");
    REQUIRE(shard.get("key1").response == "value1");
    REQUIRE(shard.get("key2").response == "value2");
}

static size_t segmentFiles(const fs::path& segDir) {
    return static_cast<size_t>(std::distance(fs::directory_iterator(segDir), fs::directory_iterator()));
}

static bool waitFor(const std::function<bool()>& condition) {
    for (int i = 0; i < 50 && !condition(); i++) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    return condition();
}

TEST_CASE_METHOD(ShardFixture, "Shard does not return a value truncated on disk", "[shard]") {
    {
        Shard shard(settings, 0);
        shard.set("key1", "a value long enough to truncate");
    }
    fs::resize_file(testDir / "shards" / "0" / "seg" / "1.seg", 5);
    Shard shard(settings, 0);
    REQUIRE(shard.get("key1").code == ResultType::Code::KeyNotFound);
}

TEST_CASE_METHOD(ShardFixture, "Shard compactor removes dead segments once every key is deleted", "[shard]") {
    settings.SegSize = 64;
    settings.CompactingInterval = 1;
    Shard shard(settings, 0);
    for (int i = 0; i < 20; i++) {
        shard.set(std::format("k{}", i), "0123456789");
    }
    for (int i = 0; i < 20; i++) {
        shard.del(std::format("k{}", i));
    }
    const fs::path segDir = testDir / "shards" / "0" / "seg";
    REQUIRE(waitFor([&] { return segmentFiles(segDir) == 0; }));
}

TEST_CASE_METHOD(ShardFixture, "Shard compactor reclaims mostly dead segments and keeps their live values", "[shard]") {
    settings.SegSize = 64;
    settings.CompactingInterval = 1;
    const fs::path seg1 = testDir / "shards" / "0" / "seg" / "1.seg";
    {
        Shard shard(settings, 0);
        for (int i = 0; i < 20; i++) {
            shard.set(std::format("k{}", i), std::format("old_{:05}", i));
        }
        for (int i = 1; i < 20; i++) {
            shard.set(std::format("k{}", i), std::format("new_{:05}", i));
        }
        REQUIRE(waitFor([&] { return !fs::exists(seg1); }));
        REQUIRE(shard.get("k0").response == "old_00000");
    }
    Shard reopened(settings, 0);
    REQUIRE(reopened.get("k0").response == "old_00000");
    for (int i = 1; i < 20; i++) {
        REQUIRE(reopened.get(std::format("k{}", i)).response == std::format("new_{:05}", i));
    }
}

TEST_CASE_METHOD(ShardFixture, "Shard setBy keeps the expiry of each key", "[shard][ttl]") {
    Shard shard(settings, 0);
    const uint64_t expiresAt = nowMs() + 60'000;
    shard.set("abc_ttl", "old", expiresAt);
    shard.set("abc_none", "old");
    ValidationRule vr;
    vr.func = [](std::string_view k) { return k.starts_with("abc_"); };

    REQUIRE(shard.setBy(vr, "new").affectedRows == 2);
    REQUIRE(shard.get("abc_ttl").response == "new");
    REQUIRE(std::stoi(shard.getTTL("abc_ttl").response) > 0);
    REQUIRE(shard.getTTL("abc_none").response == "-1");
}

TEST_CASE_METHOD(ShardFixture, "Shard replace keeps the expiry and never creates a key", "[shard][ttl]") {
    Shard shard(settings, 0);
    const uint64_t original = nowMs() + 60'000;
    shard.set("k", "old", original);

    uint64_t expiresAt = 0;
    REQUIRE(shard.replace("k", "new", expiresAt).code == ResultType::Code::Success);
    REQUIRE(expiresAt == original);
    REQUIRE(shard.get("k").response == "new");
    REQUIRE(std::stoi(shard.getTTL("k").response) > 0);

    REQUIRE(shard.replace("missing", "v", expiresAt).code == ResultType::Code::KeyNotFound);
    REQUIRE(shard.get("missing").code == ResultType::Code::KeyNotFound);
}

TEST_CASE_METHOD(ShardFixture, "Shard deadBytes counts overwritten and deleted values", "[shard][compaction]") {
    Shard shard(settings, 0);
    shard.set("a", "0123456789");
    shard.set("b", "0123456789");
    REQUIRE(shard.deadBytes() == 0);
    shard.set("a", "xyz");
    REQUIRE(shard.deadBytes() == 10);
    shard.del("b");
    REQUIRE(shard.deadBytes() == 20);
}

TEST_CASE_METHOD(ShardFixture, "Shard compactor starts once half of the data is dead, without waiting for the interval", "[shard][compaction]") {
    settings.SegSize = 64;
    settings.CompactingInterval = 3600;
    Shard shard(settings, 0);
    for (int i = 0; i < 20; i++) {
        shard.set(std::format("k{}", i), "0123456789");
    }
    for (int i = 0; i < 20; i++) {
        shard.del(std::format("k{}", i));
    }
    const fs::path segDir = testDir / "shards" / "0" / "seg";
    REQUIRE(waitFor([&] { return segmentFiles(segDir) == 0; }));
    REQUIRE(shard.deadBytes() == 0);
}

TEST_CASE_METHOD(ShardFixture, "Shard compactor leaves a shard with little dead data alone", "[shard][compaction]") {
    settings.SegSize = 64;
    settings.CompactingInterval = 3600;
    Shard shard(settings, 0);
    for (int i = 0; i < 20; i++) {
        shard.set(std::format("k{}", i), "0123456789");
    }
    shard.del("k0");
    const fs::path segDir = testDir / "shards" / "0" / "seg";
    const size_t before = segmentFiles(segDir);
    std::this_thread::sleep_for(std::chrono::milliseconds(2500));
    REQUIRE(segmentFiles(segDir) == before);
    REQUIRE(shard.deadBytes() == 10);
}
