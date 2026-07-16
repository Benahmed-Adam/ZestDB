#include <catch2/catch_all.hpp>
#include <filesystem>

#include "DataSegment.hpp"
#include "Settings.hpp"

using namespace Zest;
namespace fs = std::filesystem;

class DataSegmentFixture {
public:
    Settings settings;
    fs::path testDir;
    DataSegmentFixture() {
        testDir = fs::temp_directory_path() / ("zestdb_test_ds_" + std::to_string(reinterpret_cast<uintptr_t>(this)));
        fs::create_directories(testDir / "seg");
        settings.DbPath = testDir;
        settings.SegSize = 1024;
    }
    ~DataSegmentFixture() { fs::remove_all(testDir); }
};

TEST_CASE_METHOD(DataSegmentFixture, "DataSegment write normal", "[datasegment]") {
    DataSegment seg(settings, 1);
    unsigned long offset = seg.write("hello");
    REQUIRE(offset == 0);
    REQUIRE_FALSE(seg.isFull());
}

TEST_CASE_METHOD(DataSegmentFixture, "DataSegment write and read", "[datasegment]") {
    DataSegment seg(settings, 1);
    unsigned long offset = seg.write("hello world");
    std::string readBack = seg.read(offset, 11);
    REQUIRE(readBack == "hello world");
}

TEST_CASE_METHOD(DataSegmentFixture, "DataSegment segment full returns SegSize+1", "[datasegment]") {
    Settings small;
    small.DbPath = testDir;
    small.SegSize = 5;
    DataSegment seg(small, 1);
    unsigned long result = seg.write("123456");
    REQUIRE(result == small.SegSize + 1);
}

TEST_CASE_METHOD(DataSegmentFixture, "DataSegment write empty string", "[datasegment]") {
    DataSegment seg(settings, 1);
    unsigned long offset = seg.write("");
    REQUIRE(offset == 0);
    REQUIRE(seg.getWritePosition() == 0);
}

TEST_CASE_METHOD(DataSegmentFixture, "DataSegment isFull states", "[datasegment]") {
    Settings small;
    small.DbPath = testDir;
    small.SegSize = 5;
    DataSegment seg(small, 1);
    REQUIRE_FALSE(seg.isFull());
    seg.write("1234");
    REQUIRE_FALSE(seg.isFull());
}

TEST_CASE_METHOD(DataSegmentFixture, "DataSegment getSegmentId", "[datasegment]") {
    DataSegment seg(settings, 42);
    REQUIRE(seg.getSegmentId() == 42);
}

TEST_CASE_METHOD(DataSegmentFixture, "DataSegment read beyond EOF returns partial", "[datasegment]") {
    DataSegment seg(settings, 1);
    unsigned long offset = seg.write("hi");
    std::string readBack = seg.read(offset, 100);
    REQUIRE(readBack == "hi");
}

TEST_CASE_METHOD(DataSegmentFixture, "DataSegment multiple writes", "[datasegment]") {
    DataSegment seg(settings, 1);
    unsigned long o1 = seg.write("aaa");
    unsigned long o2 = seg.write("bbb");
    unsigned long o3 = seg.write("ccc");
    REQUIRE(o1 == 0);
    REQUIRE(o2 == 3);
    REQUIRE(o3 == 6);
    REQUIRE(seg.read(o1, 3) == "aaa");
    REQUIRE(seg.read(o2, 3) == "bbb");
    REQUIRE(seg.read(o3, 3) == "ccc");
}

TEST_CASE_METHOD(DataSegmentFixture, "DataSegment refreshFullStatus on existing segment", "[datasegment]") {
    {
        DataSegment seg(settings, 1);
        seg.write("data");
    }
    DataSegment seg2(settings, 1);
    REQUIRE(seg2.getWritePosition() == 4);
}

TEST_CASE_METHOD(DataSegmentFixture, "DataSegment flush with changes", "[datasegment]") {
    DataSegment seg(settings, 1);
    seg.write("test");
    REQUIRE_NOTHROW(seg.flush());
}

TEST_CASE_METHOD(DataSegmentFixture, "DataSegment flush without changes", "[datasegment]") {
    DataSegment seg(settings, 1);
    REQUIRE_NOTHROW(seg.flush());
}
