#include <catch2/catch_all.hpp>
#include <filesystem>
#include <fstream>

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

    ~StorageManagerFixture() {
        fs::remove_all(testDir);
    }
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
    Settings smallSettings;
    smallSettings.DbPath = testDir;
    smallSettings.SegSize = 10;
    StorageManager sm(smallSettings);
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

TEST_CASE_METHOD(StorageManagerFixture, "StorageManager rotates to new segment when remaining space is too small", "[storagemanager]") {
    Settings smallSettings;
    smallSettings.DbPath = testDir;
    smallSettings.SegSize = 10;
    StorageManager sm(smallSettings);

    sm.append("12345678");
    auto e2 = sm.append("abcd");
    REQUIRE(e2.segmentId == 2);
    REQUIRE(sm.read(e2) == "abcd");
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

TEST_CASE_METHOD(StorageManagerFixture, "StorageManager ignores .seg files that are not numbered", "[storagemanager]") {
    IndexEntry entry;
    {
        StorageManager sm(settings);
        entry = sm.append("value");
    }
    for (const char* name : { "backup.seg", "12abc.seg" }) {
        std::ofstream(testDir / "seg" / name) << "junk";
    }
    std::unique_ptr<StorageManager> sm;
    REQUIRE_NOTHROW(sm = std::make_unique<StorageManager>(settings));
    REQUIRE(sm->read(entry) == "value");
}

TEST_CASE_METHOD(StorageManagerFixture, "StorageManager compresses values that shrink and reads them back", "[storagemanager][compression]") {
    StorageManager sm(settings);
    const std::string value(200, 'z');
    auto entry = sm.append(value);
    REQUIRE(entry.segmentId != INVALID_SEGMENT_ID);
    REQUIRE(entry.size < value.size());
    REQUIRE(sm.read(entry) == value);
}

TEST_CASE_METHOD(StorageManagerFixture, "StorageManager stores short and incompressible values as is", "[storagemanager][compression]") {
    StorageManager sm(settings);
    const std::string shortValue = "abc";
    const std::string noisy = "q8#Lz!0vK@m2Xw$9";
    auto e1 = sm.append(shortValue);
    auto e2 = sm.append(noisy);
    REQUIRE(e1.size == shortValue.size());
    REQUIRE(e2.size == noisy.size());
    REQUIRE(sm.read(e1) == shortValue);
    REQUIRE(sm.read(e2) == noisy);
}

TEST_CASE_METHOD(StorageManagerFixture, "StorageManager keeps a value that starts like a zstd frame", "[storagemanager][compression]") {
    StorageManager sm(settings);
    const std::string value = std::string("\x28\xB5\x2F\xFD", 4) + "raw";
    auto entry = sm.append(value);
    REQUIRE(sm.read(entry) == value);
}

TEST_CASE_METHOD(StorageManagerFixture, "StorageManager returns an empty string for a corrupted frame", "[storagemanager][compression]") {
    IndexEntry entry;
    {
        StorageManager sm(settings);
        entry = sm.append(std::string(200, 'z'));
        sm.flush();
    }
    std::fstream seg(testDir / "seg" / "1.seg", std::ios::in | std::ios::out | std::ios::binary);
    seg.seekp(static_cast<std::streamoff>(entry.size) - 2);
    seg.put('\xFF').put('\xFF');
    seg.close();
    StorageManager sm(settings);
    REQUIRE(sm.read(entry).empty());
}

TEST_CASE_METHOD(StorageManagerFixture, "StorageManager refuses a value larger than a segment even if it compresses", "[storagemanager][compression]") {
    StorageManager sm(settings);
    REQUIRE(sm.append(std::string(settings.SegSize + 1, 'z')).segmentId == INVALID_SEGMENT_ID);
}

TEST_CASE_METHOD(StorageManagerFixture, "StorageManager totalBytes sums every segment", "[storagemanager][compaction]") {
    Settings smallSettings;
    smallSettings.DbPath = testDir;
    smallSettings.SegSize = 10;
    StorageManager sm(smallSettings);
    REQUIRE(sm.totalBytes() == 0);
    sm.append("12345678");
    sm.append("abcd");
    REQUIRE(sm.totalBytes() == 12);
    sm.removeUnusedSegments({ 2 });
    REQUIRE(sm.totalBytes() == 4);
}

TEST_CASE_METHOD(StorageManagerFixture, "StorageManager refuses a value that fails its checksum", "[storagemanager][crc]") {
    IndexEntry entry;
    {
        StorageManager sm(settings);
        entry = sm.append("plain value");
        REQUIRE(sm.read(entry) == "plain value");
        sm.flush();
    }
    {
        std::fstream seg(testDir / "seg" / "1.seg", std::ios::in | std::ios::out | std::ios::binary);
        seg.seekp(3);
        seg.put('#');
    }
    StorageManager sm(settings);
    REQUIRE(sm.read(entry).empty());
}
