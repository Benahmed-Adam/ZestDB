#include <catch2/catch_all.hpp>
#include <filesystem>

#include "WAL.hpp"

using namespace Zest;
namespace fs = std::filesystem;

class WALFixture {
public:
    Settings settings;
    fs::path testDir;
    fs::path walPath;
    WALFixture() {
        settings.fsyncOnWrite = false;
        testDir = fs::temp_directory_path() / ("zestdb_test_wal_" + std::to_string(reinterpret_cast<uintptr_t>(this)));
        fs::create_directories(testDir);
        walPath = testDir / "WAL";
    }
    ~WALFixture() { fs::remove_all(testDir); }
};

TEST_CASE_METHOD(WALFixture, "WAL constructor creates new file", "[wal]") {
    WAL wal(settings, walPath);
    auto cmds = wal.getCmds();
    REQUIRE(cmds.empty());
}

TEST_CASE_METHOD(WALFixture, "WAL append and getCmds single entry", "[wal]") {
    WAL wal(settings, walPath);
    wal.append("s key1 value1");
    auto cmds = wal.getCmds();
    REQUIRE(cmds.size() == 1);
    REQUIRE(cmds[0].cmd == "s key1 value1");
}

TEST_CASE_METHOD(WALFixture, "WAL append empty command", "[wal]") {
    WAL wal(settings, walPath);
    wal.append("");
    auto cmds = wal.getCmds();
    REQUIRE(cmds.size() == 1);
    REQUIRE(cmds[0].cmd.empty());
}

TEST_CASE_METHOD(WALFixture, "WAL append long command", "[wal]") {
    WAL wal(settings, walPath);
    std::string longCmd(10000, 'x');
    wal.append(longCmd);
    auto cmds = wal.getCmds();
    REQUIRE(cmds.size() == 1);
    REQUIRE(cmds[0].cmd == longCmd);
}

TEST_CASE_METHOD(WALFixture, "WAL getCmds empty WAL", "[wal]") {
    WAL wal(settings, walPath);
    auto cmds = wal.getCmds();
    REQUIRE(cmds.empty());
}

TEST_CASE_METHOD(WALFixture, "WAL getCmds multiple entries ordered by timestamp", "[wal]") {
    WAL wal(settings, walPath);
    wal.append("cmd1");
    wal.append("cmd2");
    wal.append("cmd3");
    auto cmds = wal.getCmds();
    REQUIRE(cmds.size() == 3);
    for (size_t i = 1; i < cmds.size(); i++) {
        REQUIRE(cmds[i].timestamp >= cmds[i - 1].timestamp);
    }
}

TEST_CASE_METHOD(WALFixture, "WAL clear empties the file", "[wal]") {
    WAL wal(settings, walPath);
    wal.append("test");
    wal.clear();
    auto cmds = wal.getCmds();
    REQUIRE(cmds.empty());
}

TEST_CASE_METHOD(WALFixture, "WAL double clear is no-op", "[wal]") {
    WAL wal(settings, walPath);
    wal.append("test");
    wal.clear();
    wal.clear();
    auto cmds = wal.getCmds();
    REQUIRE(cmds.empty());
}

TEST_CASE_METHOD(WALFixture, "WAL clear then append then clear", "[wal]") {
    WAL wal(settings, walPath);
    wal.clear();
    wal.append("newcmd");
    wal.clear();
    auto cmds = wal.getCmds();
    REQUIRE(cmds.empty());
}

TEST_CASE_METHOD(WALFixture, "WAL preserves command with newlines", "[wal]") {
    WAL wal(settings, walPath);
    wal.append("line1\nline2");
    auto cmds = wal.getCmds();
    REQUIRE(cmds.size() == 1);
    REQUIRE(cmds[0].cmd == "line1\nline2");
}
