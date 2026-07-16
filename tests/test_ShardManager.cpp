#include <catch2/catch_all.hpp>
#include <filesystem>
#include "ShardManager.hpp"

using namespace Zest;
namespace fs = std::filesystem;

class ShardManagerFixture {
public:
    Settings settings;
    fs::path testDir;
    ShardManagerFixture() {
        testDir = fs::temp_directory_path() / ("zestdb_test_smgr_" + std::to_string(reinterpret_cast<uintptr_t>(this)));
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
    ~ShardManagerFixture() { fs::remove_all(testDir); }
};

TEST_CASE_METHOD(ShardManagerFixture, "ShardManager set and get", "[shardmanager]") {
    ShardManager sm(settings, 4);
    sm.set("key1", "value1");
    auto result = sm.get("key1");
    REQUIRE(result.code == ResultType::Code::SUCCESS);
    REQUIRE(result.response == "value1");
}

TEST_CASE_METHOD(ShardManagerFixture, "ShardManager del", "[shardmanager]") {
    ShardManager sm(settings, 4);
    sm.set("key1", "value1");
    sm.del("key1");
    auto result = sm.get("key1");
    REQUIRE(result.code == ResultType::Code::ERROR);
}

TEST_CASE_METHOD(ShardManagerFixture, "ShardManager get non-existent", "[shardmanager]") {
    ShardManager sm(settings, 4);
    auto result = sm.get("ghost");
    REQUIRE(result.code == ResultType::Code::ERROR);
}

TEST_CASE_METHOD(ShardManagerFixture, "ShardManager getBy parallel merged results", "[shardmanager]") {
    ShardManager sm(settings, 4);
    sm.set("abc_1", "v1");
    sm.set("abc_2", "v2");
    sm.set("xyz_1", "v3");
    ValidationRule vr;
    vr.func = [](const std::string &k) { return k.find("abc_") == 0; };
    vr.limit = UINT_MAX;
    auto result = sm.getBy(vr);
    REQUIRE(result.code == ResultType::Code::SUCCESS);
    REQUIRE(result.affectedRows == 2);
}

TEST_CASE_METHOD(ShardManagerFixture, "ShardManager setBy parallel", "[shardmanager]") {
    ShardManager sm(settings, 4);
    sm.set("abc_1", "old");
    sm.set("abc_2", "old");
    ValidationRule vr;
    vr.func = [](const std::string &k) { return k.find("abc_") == 0; };
    vr.limit = UINT_MAX;
    sm.setBy(vr, "new");
    REQUIRE(sm.get("abc_1").response == "new");
    REQUIRE(sm.get("abc_2").response == "new");
}

TEST_CASE_METHOD(ShardManagerFixture, "ShardManager delBy parallel", "[shardmanager]") {
    ShardManager sm(settings, 4);
    sm.set("abc_1", "v1");
    sm.set("abc_2", "v2");
    sm.set("xyz_1", "v3");
    ValidationRule vr;
    vr.func = [](const std::string &k) { return k.find("abc_") == 0; };
    vr.limit = UINT_MAX;
    sm.delBy(vr);
    REQUIRE(sm.get("abc_1").code == ResultType::Code::ERROR);
    REQUIRE(sm.get("abc_2").code == ResultType::Code::ERROR);
    REQUIRE(sm.get("xyz_1").code == ResultType::Code::SUCCESS);
}

TEST_CASE_METHOD(ShardManagerFixture, "ShardManager flush", "[shardmanager]") {
    ShardManager sm(settings, 4);
    sm.set("key1", "val1");
    REQUIRE_NOTHROW(sm.flush());
}

TEST_CASE_METHOD(ShardManagerFixture, "ShardManager clearAllWAL", "[shardmanager]") {
    ShardManager sm(settings, 4);
    sm.set("key1", "val1");
    REQUIRE_NOTHROW(sm.clearAllWAL());
}

TEST_CASE_METHOD(ShardManagerFixture, "ShardManager replayAllWAL", "[shardmanager]") {
    ShardManager sm(settings, 2);
    sm.set("key1", "val1");
    sm.flush();
    std::vector<std::string> replayed;
    sm.replayAllWAL([&](const std::string &cmd) { replayed.push_back(cmd); return "ok"; });
    sm.flush();
    SUCCEED();
}

TEST_CASE_METHOD(ShardManagerFixture, "ShardManager getBy with global limit", "[shardmanager]") {
    ShardManager sm(settings, 4);
    for (int i = 0; i < 20; i++) {
        sm.set("item_" + std::to_string(i), "val");
    }
    ValidationRule vr;
    vr.func = [](const std::string &) { return true; };
    vr.limit = 5;
    auto result = sm.getBy(vr);
    REQUIRE(result.affectedRows >= 5);
    REQUIRE(result.affectedRows <= 5 + 3);
}

TEST_CASE_METHOD(ShardManagerFixture, "ShardManager routing consistency", "[shardmanager]") {
    ShardManager sm(settings, 4);
    sm.set("consistent_key", "val1");
    auto r1 = sm.get("consistent_key");
    auto r2 = sm.get("consistent_key");
    REQUIRE(r1.response == r2.response);
}
