#include <catch2/catch_all.hpp>
#include <filesystem>
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
    ~ShardFixture() { fs::remove_all(testDir); }
};

TEST_CASE_METHOD(ShardFixture, "Shard set and get", "[shard]") {
    Shard shard(settings, 0);
    auto setResult = shard.set("key1", "value1");
    REQUIRE(setResult.code == ResultType::Code::SUCCESS);
    auto getResult = shard.get("key1");
    REQUIRE(getResult.code == ResultType::Code::SUCCESS);
    REQUIRE(getResult.response == "value1");
}

TEST_CASE_METHOD(ShardFixture, "Shard get key not found", "[shard]") {
    Shard shard(settings, 0);
    auto result = shard.get("nonexistent");
    REQUIRE(result.code == ResultType::Code::ERROR);
}

TEST_CASE_METHOD(ShardFixture, "Shard del existing key", "[shard]") {
    Shard shard(settings, 0);
    shard.set("key1", "value1");
    auto delResult = shard.del("key1");
    REQUIRE(delResult.code == ResultType::Code::SUCCESS);
    auto getResult = shard.get("key1");
    REQUIRE(getResult.code == ResultType::Code::ERROR);
}

TEST_CASE_METHOD(ShardFixture, "Shard del key not found", "[shard]") {
    Shard shard(settings, 0);
    auto result = shard.del("ghost");
    REQUIRE(result.code == ResultType::Code::ERROR);
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
    REQUIRE(result.code == ResultType::Code::SUCCESS);
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
    REQUIRE(result.code == ResultType::Code::SUCCESS);
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
    REQUIRE(result.code == ResultType::Code::SUCCESS);
    REQUIRE(result.affectedRows == 2);
    REQUIRE(shard.get("abc_1").code == ResultType::Code::ERROR);
    REQUIRE(shard.get("xyz_1").code == ResultType::Code::SUCCESS);
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
        REQUIRE(r.code == ResultType::Code::SUCCESS);
        REQUIRE(r.response == "val_" + std::to_string(i));
    }
}

TEST_CASE_METHOD(ShardFixture, "Shard WAL append works", "[shard]") {
    Shard shard(settings, 0);
    auto &wal = shard.getWAL();
    wal.append("set key1 val1");
    auto cmds = wal.getCmds();
    REQUIRE_FALSE(cmds.empty());
    REQUIRE(cmds[0].cmd == "set key1 val1");
}
