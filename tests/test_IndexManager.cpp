#include <catch2/catch_all.hpp>
#include <filesystem>

#include "IndexManager.hpp"

using namespace Zest;
namespace fs = std::filesystem;

class IndexManagerFixture {
public:
    Settings settings;
    fs::path testDir;
    IndexManagerFixture() {
        testDir = fs::temp_directory_path() / ("zestdb_test_im_" + std::to_string(reinterpret_cast<uintptr_t>(this)));
        fs::create_directories(testDir);
        settings.DbPath = testDir;
        settings.IndexPath = testDir / "INDEX";
        settings.SegSize = 100000;
    }
    ~IndexManagerFixture() { fs::remove_all(testDir); }
};

static IndexEntry makeEntry(const std::string &key, uint32_t segId = 1, unsigned long offset = 0) {
    IndexEntry e{};
    e.key = key;
    e.segmentId = segId;
    e.offset = offset;
    e.size = static_cast<unsigned int>(key.size());
    e.isTombstone = false;
    return e;
}

TEST_CASE_METHOD(IndexManagerFixture, "IndexManager insert and search", "[indexmanager]") {
    IndexManager im(settings);
    im.insert(makeEntry("key1", 1, 0));
    auto result = im.search("key1");
    REQUIRE(result.segmentId == 1);
    REQUIRE(result.key == "key1");
}

TEST_CASE_METHOD(IndexManagerFixture, "IndexManager search key not found", "[indexmanager]") {
    IndexManager im(settings);
    auto result = im.search("nonexistent");
    REQUIRE(result.segmentId == INVALID_SEGMENT_ID);
}

TEST_CASE_METHOD(IndexManagerFixture, "IndexManager insert duplicate key updates", "[indexmanager]") {
    IndexManager im(settings);
    im.insert(makeEntry("key1", 1, 0));
    im.insert(makeEntry("key1", 2, 100));
    auto result = im.search("key1");
    REQUIRE(result.segmentId == 2);
    REQUIRE(result.offset == 100);
}

TEST_CASE_METHOD(IndexManagerFixture, "IndexManager update existing key", "[indexmanager]") {
    IndexManager im(settings);
    im.insert(makeEntry("key1", 1, 0));
    IndexEntry updated = makeEntry("key1", 3, 50);
    im.update("key1", updated);
    auto result = im.search("key1");
    REQUIRE(result.segmentId == 3);
    REQUIRE(result.offset == 50);
}

TEST_CASE_METHOD(IndexManagerFixture, "IndexManager update non-existent key is no-op", "[indexmanager]") {
    IndexManager im(settings);
    im.update("ghost", makeEntry("ghost", 1, 0));
    auto result = im.search("ghost");
    REQUIRE(result.segmentId == INVALID_SEGMENT_ID);
}

TEST_CASE_METHOD(IndexManagerFixture, "IndexManager update to tombstone", "[indexmanager]") {
    IndexManager im(settings);
    im.insert(makeEntry("key1", 1, 0));
    IndexEntry tomb = makeEntry("key1", 1, 0);
    tomb.isTombstone = true;
    im.update("key1", tomb);
    auto result = im.search("key1");
    REQUIRE(result.segmentId == INVALID_SEGMENT_ID);
}

TEST_CASE_METHOD(IndexManagerFixture, "IndexManager getAll empty index", "[indexmanager]") {
    IndexManager im(settings);
    auto entries = im.getAll();
    REQUIRE(entries.empty());
}

TEST_CASE_METHOD(IndexManagerFixture, "IndexManager getAll with limit", "[indexmanager]") {
    IndexManager im(settings);
    im.insert(makeEntry("a", 1, 0));
    im.insert(makeEntry("b", 1, 10));
    im.insert(makeEntry("c", 1, 20));
    auto entries = im.getAll(2);
    REQUIRE(entries.size() == 2);
}

TEST_CASE_METHOD(IndexManagerFixture, "IndexManager getAll limit exceeds entries", "[indexmanager]") {
    IndexManager im(settings);
    im.insert(makeEntry("a", 1, 0));
    auto entries = im.getAll(100);
    REQUIRE(entries.size() == 1);
}

TEST_CASE_METHOD(IndexManagerFixture, "IndexManager compact removes tombstones", "[indexmanager]") {
    IndexManager im(settings);
    im.insert(makeEntry("a", 1, 0));
    im.insert(makeEntry("b", 1, 10));
    IndexEntry tomb = makeEntry("a", 1, 0);
    tomb.isTombstone = true;
    im.update("a", tomb);
    auto result = im.compact();
    REQUIRE(result.size() == 1);
    REQUIRE(result[0].key == "b");
}

TEST_CASE_METHOD(IndexManagerFixture, "IndexManager compact all tombstoned", "[indexmanager]") {
    IndexManager im(settings);
    im.insert(makeEntry("a", 1, 0));
    IndexEntry tomb = makeEntry("a", 1, 0);
    tomb.isTombstone = true;
    im.update("a", tomb);
    auto result = im.compact();
    REQUIRE(result.empty());
}

TEST_CASE_METHOD(IndexManagerFixture, "IndexManager flush with changes", "[indexmanager]") {
    IndexManager im(settings);
    im.insert(makeEntry("a", 1, 0));
    REQUIRE_NOTHROW(im.flush());
}

TEST_CASE_METHOD(IndexManagerFixture, "IndexManager flush without changes", "[indexmanager]") {
    IndexManager im(settings);
    REQUIRE_NOTHROW(im.flush());
}

TEST_CASE_METHOD(IndexManagerFixture, "IndexManager insert tombstone slot reuse", "[indexmanager]") {
    IndexManager im(settings);
    im.insert(makeEntry("a", 1, 0));
    IndexEntry tomb = makeEntry("a", 1, 0);
    tomb.isTombstone = true;
    im.update("a", tomb);
    im.insert(makeEntry("b", 1, 50));
    auto result = im.search("b");
    REQUIRE(result.segmentId == 1);
    REQUIRE(result.offset == 50);
}

TEST_CASE_METHOD(IndexManagerFixture, "IndexManager persistence across instances", "[indexmanager]") {
    {
        IndexManager im(settings);
        im.insert(makeEntry("persistent", 1, 42));
    }
    IndexManager im2(settings);
    auto result = im2.search("persistent");
    REQUIRE(result.segmentId == 1);
    REQUIRE(result.offset == 42);
}
