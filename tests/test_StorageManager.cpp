#include <catch2/catch_all.hpp>
#include <filesystem>

#include "Settings.hpp"
#include "StorageManager.hpp"

using namespace Zest;
namespace fs = std::filesystem;

class StorageManagerFixture {
public:
    Settings settings;
    fs::path testDir;
    StorageManagerFixture() {
        testDir = fs::temp_directory_path() / ("zestdb_test_sm_" + std::to_string(reinterpret_cast<uintptr_t>(this)));
        fs::create_directories(testDir / "seg");
        settings.DbPath = testDir;
        settings.SegSize = 256;
    }
    ~StorageManagerFixture() { fs::remove_all(testDir); }
};

TEST_CASE_METHOD(StorageManagerFixture, "StorageManager boot creates segment 1", "[storagemanager]") {
    StorageManager sm(settings);
    auto entry = sm.append("test");
    REQUIRE(entry.segmentId == 1);
    REQUIRE(entry.offset == 0);
}

TEST_CASE_METHOD(StorageManagerFixture, "StorageManager append normal", "[storagemanager]") {
    StorageManager sm(settings);
    auto e1 = sm.append("hello");
    auto e2 = sm.append("world");
    REQUIRE(e1.segmentId >= 1);
    REQUIRE(e2.segmentId >= 1);
}

TEST_CASE_METHOD(StorageManagerFixture, "StorageManager read valid segment", "[storagemanager]") {
    StorageManager sm(settings);
    auto entry = sm.append("testdata");
    std::string readBack = sm.read(entry);
    REQUIRE(readBack == "testdata");
}

TEST_CASE_METHOD(StorageManagerFixture, "StorageManager read non-existent segment", "[storagemanager]") {
    StorageManager sm(settings);
    IndexEntry fake{};
    fake.segmentId = 999;
    fake.offset = 0;
    fake.size = 4;
    std::string result = sm.read(fake);
    REQUIRE(result.empty());
}

TEST_CASE_METHOD(StorageManagerFixture, "StorageManager segment rotation", "[storagemanager]") {
    Settings small;
    small.DbPath = testDir;
    small.SegSize = 10;
    StorageManager sm(small);
    sm.append("1234567890");
    auto e2 = sm.append("new");
    REQUIRE(e2.segmentId == 2);
}

TEST_CASE_METHOD(StorageManagerFixture, "StorageManager removeUnusedSegments", "[storagemanager]") {
    StorageManager sm(settings);
    sm.append("a");
    sm.append("b");
    sm.flush();
    std::vector<uint32_t> used = { 1 };
    sm.removeUnusedSegments(used);
}

TEST_CASE_METHOD(StorageManagerFixture, "StorageManager flush", "[storagemanager]") {
    StorageManager sm(settings);
    sm.append("test");
    REQUIRE_NOTHROW(sm.flush());
}

TEST_CASE_METHOD(StorageManagerFixture, "StorageManager append multiple values read back", "[storagemanager]") {
    StorageManager sm(settings);
    auto e1 = sm.append("val1");
    auto e2 = sm.append("val2");
    auto e3 = sm.append("val3");
    REQUIRE(sm.read(e1) == "val1");
    REQUIRE(sm.read(e2) == "val2");
    REQUIRE(sm.read(e3) == "val3");
}
