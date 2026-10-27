#include <catch2/catch_all.hpp>
#include <filesystem>
#include <fstream>

#include "WAL.hpp"
#include "File.hpp"

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

    ~WALFixture() {
        fs::remove_all(testDir);
    }
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

TEST_CASE_METHOD(WALFixture, "WAL getCmds multiple entries ordered by sequence", "[wal]") {
    WAL wal(settings, walPath);
    wal.append("cmd1");
    wal.append("cmd2");
    wal.append("cmd3");
    auto cmds = wal.getCmds();
    REQUIRE(cmds.size() == 3);
    for (size_t i = 1; i < cmds.size(); i++) {
        REQUIRE(cmds[i].seq > cmds[i - 1].seq);
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

TEST_CASE_METHOD(WALFixture, "WAL append reaches the file without fsyncOnWrite", "[wal]") {
    WAL wal(settings, walPath);
    wal.append("s key value");
    REQUIRE(wal.sync());
    REQUIRE(fs::file_size(walPath) > 0);
}

TEST_CASE_METHOD(WALFixture, "WAL sequence numbers order entries across WAL files", "[wal]") {
    WAL a(settings, walPath);
    WAL b(settings, testDir / "WAL2");
    REQUIRE(a.append("a1"));
    REQUIRE(b.append("b1"));
    REQUIRE(a.append("a2"));
    auto ca = a.getCmds();
    auto cb = b.getCmds();
    REQUIRE(ca.size() == 2);
    REQUIRE(cb.size() == 1);
    REQUIRE(ca[0].seq < cb[0].seq);
    REQUIRE(cb[0].seq < ca[1].seq);
}

TEST_CASE_METHOD(WALFixture, "WAL replays a record larger than 1 MB and the ones after it", "[wal]") {
    WAL wal(settings, walPath);
    std::string big(2 * 1024 * 1024, 'x');
    REQUIRE(wal.append(big));
    REQUIRE(wal.append("after"));
    auto cmds = wal.getCmds();
    REQUIRE(cmds.size() == 2);
    REQUIRE(cmds[0].cmd == big);
    REQUIRE(cmds[1].cmd == "after");
}

TEST_CASE_METHOD(WALFixture, "WAL stops on a truncated record instead of allocating its announced size", "[wal]") {
    {
        WAL wal(settings, walPath);
        REQUIRE(wal.append("ok"));
    }
    {
        std::ofstream raw(walPath, std::ios::binary | std::ios::app);
        uint32_t hugeSize = 0xFFFFFFF0;
        raw.write(reinterpret_cast<const char*>(&hugeSize), sizeof(hugeSize));
    }
    WAL wal(settings, walPath);
    auto cmds = wal.getCmds();
    REQUIRE(cmds.size() == 1);
    REQUIRE(cmds[0].cmd == "ok");
}

TEST_CASE_METHOD(WALFixture, "flush_and_fsync reports a stream that is not open", "[wal]") {
    std::fstream closed;
    REQUIRE_FALSE(flush_and_fsync(closed, walPath));
    std::fstream open(walPath, std::ios::out | std::ios::binary);
    open << "data";
    REQUIRE(flush_and_fsync(open, walPath));
}

TEST_CASE_METHOD(WALFixture, "WAL sequence continues after the entries found at replay", "[wal]") {
    WAL::advanceSequence(1u << 30);
    WAL wal(settings, walPath);
    REQUIRE(wal.append("after replay"));
    REQUIRE(wal.getCmds().back().seq >= (1u << 30));
}
