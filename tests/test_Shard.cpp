#include <atomic>
#include <catch2/catch_all.hpp>
#include <filesystem>
#include <functional>
#include <format>
#include <fstream>
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
    REQUIRE(result.code == ResultType::Code::Error);
}

TEST_CASE_METHOD(ShardFixture, "Shard del existing key", "[shard]") {
    Shard shard(settings, 0);
    shard.set("key1", "value1");
    auto delResult = shard.del("key1");
    REQUIRE(delResult.code == ResultType::Code::Success);
    auto getResult = shard.get("key1");
    REQUIRE(getResult.code == ResultType::Code::Error);
}

TEST_CASE_METHOD(ShardFixture, "Shard del key not found", "[shard]") {
    Shard shard(settings, 0);
    auto result = shard.del("ghost");
    REQUIRE(result.code == ResultType::Code::Error);
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
    REQUIRE(shard.get("abc_1").code == ResultType::Code::Error);
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
    settings.CompactingInterval = 0;
    constexpr int writers = 4;
    constexpr int perWriter = 200;

    auto expected = [](int w, int i) -> std::string { return i % 3 == 0 ? "" : std::format("k{}_{}_v2", w, i); };
    auto checkAll = [&](Shard& shard) {
        for (int w = 0; w < writers; w++) {
            for (int i = 0; i < perWriter; i++) {
                auto r = shard.get(std::format("k{}_{}", w, i));
                if (expected(w, i).empty()) {
                    REQUIRE(r.code == ResultType::Code::Error);
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
            while (!done) {
                shard.flush();
            }
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
        checkAll(shard);
    }

    Shard reopened(settings, 0);
    checkAll(reopened);
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
    REQUIRE(shard.get("key1").code == ResultType::Code::Error);
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
