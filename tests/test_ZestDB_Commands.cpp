#include <catch2/catch_all.hpp>
#include <chrono>
#include <filesystem>
#include <format>
#include <fstream>
#include <future>
#include <libzippp/libzippp.h>
#include <thread>

#include "WAL.hpp"
#include "ZestDB.hpp"
#include "lib/json.hpp"

using namespace Zest;
namespace fs = std::filesystem;

class ZestDBCmdFixture {
public:
    fs::path testDir;
    fs::path originalCwd;
    ZestDB* db = nullptr;

    ZestDBCmdFixture() {
        originalCwd = fs::current_path();
        testDir = fs::temp_directory_path() / ("zestdb_test_cmd_" + std::to_string(reinterpret_cast<uintptr_t>(this)));
        fs::create_directories(testDir);
        fs::create_directories(testDir / "archive");

        std::ofstream cfg(testDir / "config.yaml");
        cfg << "DbPath: " << (testDir / "db").string() << "\n";
        cfg << "SegSize: 100000\n";
        cfg << "MaxKeySize: 255\n";
        cfg << "MaxValueSize: 10000\n";
        cfg << "CacheSize: 100\n";
        cfg << "CompactingInterval: 3600\n";
        cfg << "FlushInterval: 3600\n";
        cfg << "isDebug: false\n";
        cfg << "readOnly: false\n";
        cfg << "jsonOnly: false\n";
        cfg << "DBPort: 29501\n";
        cfg << "WebPort: 29502\n";
        cfg << "useSSL: false\n";
        cfg << "NetworkValidation: .*\n";
        cfg << "ArchiveStoragePath: " << (testDir / "archive").string() << "\n";
        cfg << "ArchiveCreationDelay: 3600\n";
        cfg << "AutoArchiveSaving: false\n";
        cfg.close();

        fs::current_path(testDir);
        db = new ZestDB();
    }

    ~ZestDBCmdFixture() {
        if (db) {
            db->stop();
            delete db;
        }
        fs::current_path(originalCwd);
        fs::remove_all(testDir);
    }
};

TEST_CASE_METHOD(ZestDBCmdFixture, "execCmd empty input", "[commands]") {
    auto r = db->execCmd("");
    REQUIRE(r.code == ResultType::Code::NoCommand);
}

TEST_CASE_METHOD(ZestDBCmdFixture, "execCmd whitespace only", "[commands]") {
    auto r = db->execCmd("   \t  ");
    REQUIRE(r.code == ResultType::Code::NoCommand);
}

TEST_CASE_METHOD(ZestDBCmdFixture, "execCmd unknown command", "[commands]") {
    auto r = db->execCmd("xyz");
    REQUIRE(r.code == ResultType::Code::UnknownCommand);
}

TEST_CASE_METHOD(ZestDBCmdFixture, "execCmd help", "[commands]") {
    auto r = db->execCmd("h");
    REQUIRE(r.code == ResultType::Code::Success);
}

TEST_CASE_METHOD(ZestDBCmdFixture, "execCmd set and get", "[commands]") {
    auto s = db->execCmd(R"(s key1 "value1")");
    REQUIRE(s.code == ResultType::Code::Success);
    auto g = db->execCmd("g key1");
    REQUIRE(g.code == ResultType::Code::Success);
}

TEST_CASE_METHOD(ZestDBCmdFixture, "execCmd get missing key", "[commands]") {
    auto r = db->execCmd("g");
    REQUIRE(r.code == ResultType::Code::MissingKey);
}

TEST_CASE_METHOD(ZestDBCmdFixture, "execCmd set missing key", "[commands]") {
    auto r = db->execCmd("s");
    REQUIRE(r.code == ResultType::Code::MissingKey);
}

TEST_CASE_METHOD(ZestDBCmdFixture, "execCmd set missing value", "[commands]") {
    auto r = db->execCmd("s key1");
    REQUIRE(r.code == ResultType::Code::MissingValue);
}

TEST_CASE_METHOD(ZestDBCmdFixture, "execCmd set preserves spaces in value", "[commands]") {
    db->execCmd(R"(s key1 "hello world foo")");
    auto g = db->execCmd("g key1");
    REQUIRE(g.response == "hello world foo");
}

TEST_CASE_METHOD(ZestDBCmdFixture, "execCmd del existing key", "[commands]") {
    db->execCmd(R"(s key1 "val")");
    auto r = db->execCmd("d key1");
    REQUIRE(r.code == ResultType::Code::Success);
}

TEST_CASE_METHOD(ZestDBCmdFixture, "execCmd del missing key", "[commands]") {
    auto r = db->execCmd("d");
    REQUIRE(r.code == ResultType::Code::MissingKey);
}

TEST_CASE_METHOD(ZestDBCmdFixture, "execCmd getby missing args", "[commands]") {
    auto r = db->execCmd("gb");
    REQUIRE(r.code == ResultType::Code::MissingArguments);
}

TEST_CASE_METHOD(ZestDBCmdFixture, "execCmd getby valid re", "[commands]") {
    db->execCmd(R"(s test_1 "v1")");
    db->execCmd(R"(s test_2 "v2")");
    auto r = db->execCmd("gb re test_.*");
    REQUIRE(r.code == ResultType::Code::Success);
    REQUIRE(r.affectedRows == 2);
}

TEST_CASE_METHOD(ZestDBCmdFixture, "execCmd getby invalid mode", "[commands]") {
    auto r = db->execCmd("gb xx pat");
    REQUIRE(r.code == ResultType::Code::InvalidRegex);
}

TEST_CASE_METHOD(ZestDBCmdFixture, "execCmd getby with lim", "[commands]") {
    db->execCmd(R"(s a1 "v1")");
    db->execCmd(R"(s a2 "v2")");
    db->execCmd(R"(s a3 "v3")");
    auto r = db->execCmd("gb sw a lim 2");
    REQUIRE(r.code == ResultType::Code::Success);
    REQUIRE(r.affectedRows == 2);
}

TEST_CASE_METHOD(ZestDBCmdFixture, "execCmd setby missing value", "[commands]") {
    auto r = db->execCmd("sb re .*");
    REQUIRE(r.code == ResultType::Code::MissingValue);
}

TEST_CASE_METHOD(ZestDBCmdFixture, "execCmd setby valid", "[commands]") {
    db->execCmd(R"(s abc_1 "old")");
    db->execCmd(R"(s abc_2 "old")");
    auto r = db->execCmd(R"(sb sw abc_ "newval")");
    REQUIRE(r.code == ResultType::Code::Success);
}

TEST_CASE_METHOD(ZestDBCmdFixture, "execCmd setby preserves spaces in value", "[commands]") {
    db->execCmd(R"(s abc_1 "old")");
    db->execCmd(R"(sb sw abc_ "hello world")");
    auto g = db->execCmd("g abc_1");
    REQUIRE(g.response == "hello world");
}

TEST_CASE_METHOD(ZestDBCmdFixture, "execCmd delby missing pattern", "[commands]") {
    auto r = db->execCmd("db");
    REQUIRE(r.code == ResultType::Code::MissingPattern);
}

TEST_CASE_METHOD(ZestDBCmdFixture, "execCmd delby valid", "[commands]") {
    db->execCmd(R"(s abc_1 "v1")");
    db->execCmd(R"(s abc_2 "v2")");
    db->execCmd(R"(s xyz_1 "v3")");
    auto r = db->execCmd("db sw abc_");
    REQUIRE(r.code == ResultType::Code::Success);
    REQUIRE(r.affectedRows == 2);
}

TEST_CASE_METHOD(ZestDBCmdFixture, "execCmd delby invalid mode", "[commands]") {
    auto r = db->execCmd("db xx pat");
    REQUIRE(r.code == ResultType::Code::InvalidMode);
}

TEST_CASE_METHOD(ZestDBCmdFixture, "execCmd case insensitive", "[commands]") {
    auto r = db->execCmd(R"(S key1 "value1")");
    REQUIRE(r.code == ResultType::Code::Success);
    auto g = db->execCmd("G key1");
    REQUIRE(g.code == ResultType::Code::Success);
}

TEST_CASE_METHOD(ZestDBCmdFixture, "execCmd getconfig", "[commands]") {
    auto r = db->execCmd("gcfg");
    REQUIRE(r.code == ResultType::Code::Success);
}

TEST_CASE_METHOD(ZestDBCmdFixture, "execCmd flush", "[commands]") {
    auto r = db->execCmd("f");
    REQUIRE(r.code == ResultType::Code::Success);
    REQUIRE(r.code == ResultType::Code::Success);
}

TEST_CASE_METHOD(ZestDBCmdFixture, "execCmd scfg missing args", "[commands]") {
    auto r = db->execCmd("scfg");
    REQUIRE(r.code == ResultType::Code::MissingArguments);
}

TEST_CASE_METHOD(ZestDBCmdFixture, "execCmd scfg segsize valid", "[commands]") {
    auto r = db->execCmd("scfg segsize 200000");
    REQUIRE(r.code == ResultType::Code::Success);
}

TEST_CASE_METHOD(ZestDBCmdFixture, "execCmd scfg segsize too small", "[commands]") {
    auto r = db->execCmd("scfg segsize 5");
    REQUIRE(r.code == ResultType::Code::InvalidConfigValue);
}

TEST_CASE_METHOD(ZestDBCmdFixture, "execCmd scfg maxkeysize valid", "[commands]") {
    auto r = db->execCmd("scfg maxkeysize 128");
    REQUIRE(r.code == ResultType::Code::Success);
}

TEST_CASE_METHOD(ZestDBCmdFixture, "execCmd scfg maxkeysize too large", "[commands]") {
    auto r = db->execCmd("scfg maxkeysize 300");
    REQUIRE(r.code == ResultType::Code::InvalidConfigValue);
}

TEST_CASE_METHOD(ZestDBCmdFixture, "execCmd scfg maxvaluesize valid", "[commands]") {
    auto r = db->execCmd("scfg maxvaluesize 5000");
    REQUIRE(r.code == ResultType::Code::Success);
}

TEST_CASE_METHOD(ZestDBCmdFixture, "execCmd scfg maxvaluesize too large", "[commands]") {
    auto r = db->execCmd("scfg maxvaluesize 999999");
    REQUIRE(r.code == ResultType::Code::InvalidConfigValue);
}

TEST_CASE_METHOD(ZestDBCmdFixture, "execCmd scfg cachesize", "[commands]") {
    auto r = db->execCmd("scfg cachesize 5000");
    REQUIRE(r.code == ResultType::Code::Success);
}

TEST_CASE_METHOD(ZestDBCmdFixture, "execCmd scfg isdebug true", "[commands]") {
    auto r = db->execCmd("scfg isdebug true");
    REQUIRE(r.code == ResultType::Code::Success);
}

TEST_CASE_METHOD(ZestDBCmdFixture, "execCmd scfg isdebug false", "[commands]") {
    auto r = db->execCmd("scfg isdebug 0");
    REQUIRE(r.code == ResultType::Code::Success);
}

TEST_CASE_METHOD(ZestDBCmdFixture, "execCmd scfg isdebug invalid", "[commands]") {
    auto r = db->execCmd("scfg isdebug maybe");
    REQUIRE(r.code == ResultType::Code::InvalidConfigValue);
}

TEST_CASE_METHOD(ZestDBCmdFixture, "execCmd scfg unknown param", "[commands]") {
    auto r = db->execCmd("scfg unknown 123");
    REQUIRE(r.code == ResultType::Code::UnknownConfigParam);
}

TEST_CASE_METHOD(ZestDBCmdFixture, "execCmd scfg non-numeric int", "[commands]") {
    auto r = db->execCmd("scfg segsize abc");
    REQUIRE(r.code == ResultType::Code::InvalidConfigValue);
}

TEST_CASE_METHOD(ZestDBCmdFixture, "execCmd delby with lim", "[commands]") {
    db->execCmd(R"(s a1 "v1")");
    db->execCmd(R"(s a2 "v2")");
    db->execCmd(R"(s a3 "v3")");
    auto r = db->execCmd("db sw a lim 1");
    REQUIRE(r.code == ResultType::Code::Success);
    REQUIRE(r.affectedRows == 1);
}

TEST_CASE_METHOD(ZestDBCmdFixture, "execCmd readonly blocks set", "[commands]") {
    db->execCmd("scfg readonly true");
    auto r = db->execCmd(R"(s key1 "val")");
    REQUIRE(r.code == ResultType::Code::ReadOnly);
}

TEST_CASE_METHOD(ZestDBCmdFixture, "execCmd readonly blocks del", "[commands]") {
    db->execCmd("scfg readonly true");
    auto r = db->execCmd("d key1");
    REQUIRE(r.code == ResultType::Code::ReadOnly);
}

TEST_CASE_METHOD(ZestDBCmdFixture, "execCmd readonly allows get", "[commands]") {
    db->execCmd("scfg readonly true");
    auto r = db->execCmd("g key1");
    REQUIRE(r.code == ResultType::Code::KeyNotFound);
    REQUIRE(r.code != ResultType::Code::ReadOnly);
}

TEST_CASE_METHOD(ZestDBCmdFixture, "execCmd jsononly blocks non-json", "[commands]") {
    db->execCmd("scfg jsononly true");
    auto r = db->execCmd(R"(s key1 "plainvalue")");
    REQUIRE(r.code == ResultType::Code::JsonOnly);
}

TEST_CASE_METHOD(ZestDBCmdFixture, "execCmd jsononly allows json", "[commands]") {
    db->execCmd("scfg jsononly true");
    auto r = db->execCmd(R"(s key1 "{\"a\":1}")");
    REQUIRE(r.code == ResultType::Code::Success);
}

TEST_CASE_METHOD(ZestDBCmdFixture, "execCmd getby lim counts matches, not scanned keys", "[commands]") {
    db->set("target", "x");
    for (int i = 0; i < 500; i++) {
        db->set("k" + std::to_string(i), "v");
    }
    REQUIRE(db->execCmd("gb sw target lim 1").affectedRows == 1);
    REQUIRE(db->execCmd(R"(sb sw target "y" lim 1)").affectedRows == 1);
    REQUIRE(db->execCmd("db sw target lim 1").affectedRows == 1);
}

TEST_CASE_METHOD(ZestDBCmdFixture, "execCmd invalid lim is an error, not an exception", "[commands]") {
    for (const char* cmd : { "gb sw a lim abc", R"(sb sw a "val" lim x)", "db sw a lim -1" }) {
        ResultType r;
        REQUIRE_NOTHROW(r = db->execCmd(cmd));
        REQUIRE(r.code == ResultType::Code::InvalidLimit);
    }
}

TEST_CASE_METHOD(ZestDBCmdFixture, "execCmd set key that appears earlier in the command", "[commands]") {
    db->execCmd(R"(s s "hello")");
    REQUIRE(db->execCmd("g s").response == "hello");
}

TEST_CASE_METHOD(ZestDBCmdFixture, "execCmd setby value equal to pattern", "[commands]") {
    db->execCmd(R"(s user1 "a")");
    db->execCmd(R"(sb sw user "user")");
    REQUIRE(db->execCmd("g user1").response == "user");
}

TEST_CASE_METHOD(ZestDBCmdFixture, "execCmd accepts long command names", "[commands]") {
    REQUIRE(db->execCmd(R"(set key1 "v1")").code == ResultType::Code::Success);
    REQUIRE(db->execCmd("GET key1").response == "v1");
    REQUIRE(db->execCmd("getby sw key").affectedRows == 1);
    REQUIRE(db->execCmd(R"(setby sw key "v2")").affectedRows == 1);
    REQUIRE(db->execCmd("delby sw key").affectedRows == 1);
    REQUIRE(db->execCmd("del key1").code == ResultType::Code::KeyNotFound);
    REQUIRE(db->execCmd("flush").code == ResultType::Code::Success);
    REQUIRE(db->execCmd("help").code == ResultType::Code::Success);
}

TEST_CASE("ZestDB missing config fields fall back to defaults", "[commands]") {
    fs::path originalCwd = fs::current_path();
    fs::path testDir = fs::temp_directory_path() / "zestdb_test_cfg_defaults";
    fs::remove_all(testDir);
    fs::create_directories(testDir);
    {
        std::ofstream cfg(testDir / "config.yaml");
        cfg << "DbPath: " << (testDir / "db").string() << "\n";
        cfg << "DBPort: 29511\n";
        cfg << "WebPort: 29512\n";
        cfg << "ArchiveStoragePath: " << (testDir / "archive").string() << "\n";
        cfg << "AutoArchiveSaving: false\n";
    }
    fs::current_path(testDir);
    {
        ZestDB db;
        auto cfg = nlohmann::json::parse(db.execCmd("gcfg").response);
        db.stop();
        REQUIRE(cfg["MaxKeySize"] == 64);
        REQUIRE(cfg["MaxValueSize"] == 10000);
        REQUIRE(cfg["SegSize"] == 128000);
        REQUIRE(cfg["CacheSize"] == 1000);
        REQUIRE(cfg["FlushInterval"] == 120);
        REQUIRE(cfg["fsyncOnWrite"] == true);
        REQUIRE(cfg["NetworkValidation"] == ".*");
    }
    fs::current_path(originalCwd);
    fs::remove_all(testDir);
}

TEST_CASE_METHOD(ZestDBCmdFixture, "execCmd setby takes a trailing lim, as documented", "[commands]") {
    for (const char* k : { "k1", "k2", "k3" }) {
        db->set(k, "old");
    }
    REQUIRE(db->execCmd(R"(sb sw k "hello world" lim 2)").affectedRows == 2);
    int updated = 0;
    for (const char* k : { "k1", "k2", "k3" }) {
        updated += db->get(k).response == "hello world";
    }
    REQUIRE(updated == 2);
}

TEST_CASE_METHOD(ZestDBCmdFixture, "execCmd getby regex without catastrophic backtracking", "[commands]") {
    db->set(std::string(30, 'a') + "b", "v");
    auto start = std::chrono::steady_clock::now();
    auto r = db->execCmd("gb re (a+)+$");
    REQUIRE(r.code == ResultType::Code::Success);
    REQUIRE(r.affectedRows == 0);
    REQUIRE(std::chrono::steady_clock::now() - start < std::chrono::seconds(1));
}

TEST_CASE_METHOD(ZestDBCmdFixture, "execCmd getby regex refuses back-references", "[commands]") {
    REQUIRE(db->execCmd(R"(gb re (a)\1)").code == ResultType::Code::InvalidRegex);
}

class TempDbDir {
public:
    fs::path dir;
    fs::path originalCwd = fs::current_path();

    TempDbDir(const std::string& name, const std::string& extraConfig, int port) {
        dir = fs::temp_directory_path() / name;
        fs::remove_all(dir);
        fs::create_directories(dir);
        writeConfig(std::format("DbPath: {}\nMaxKeySize: 255\nDBPort: {}\nWebPort: {}\nArchiveStoragePath: {}\nAutoArchiveSaving: false\n{}", (dir / "db").string(), port, port + 1, (dir / "archive").string(), extraConfig));
        fs::current_path(dir);
    }

    void writeConfig(const std::string& text) {
        std::ofstream(dir / "config.yaml") << text;
    }

    ~TempDbDir() {
        fs::current_path(originalCwd);
        fs::remove_all(dir);
    }
};

TEST_CASE("ZestDB WAL replay keeps writes that today's config would reject", "[commands]") {
    TempDbDir tmp("zestdb_test_replay_policy", "readOnly: true\nMaxValueSize: 5\n", 29531);
    fs::create_directories(tmp.dir / "db" / "shards" / "0");
    {
        Settings s;
        s.fsyncOnWrite = false;
        WAL wal(s, tmp.dir / "db" / "shards" / "0" / "WAL");
        REQUIRE(wal.append(R"(s replayed "a value longer than five")"));
    }
    ZestDB db;
    REQUIRE(db.get("replayed").response == "a value longer than five");
}

TEST_CASE("ZestDB reload with a broken config keeps the users", "[commands]") {
    TempDbDir tmp("zestdb_test_reload_users", "users:\n  - user: alice\n    password: secret\n", 29533);
    ZestDB db;
    REQUIRE(db.validateToken("alice", sha256("alicesecret")));
    tmp.writeConfig("users: [\n  - user: {\n");
    REQUIRE(db.execCmd("r").code == ResultType::Code::ReloadFailed);
    REQUIRE(db.validateToken("alice", sha256("alicesecret")));
}

TEST_CASE("ZestDB accepts ports above 32767", "[commands]") {
    TempDbDir tmp("zestdb_test_high_port", "", 40123);
    ZestDB db;
    auto cfg = nlohmann::json::parse(db.execCmd("gcfg").response);
    REQUIRE(cfg["DBPort"] == 40123);
    REQUIRE(cfg["WebPort"] == 40124);
}

TEST_CASE("ZestDB refuses an interval of 0 seconds in the config", "[commands]") {
    TempDbDir tmp("zestdb_test_zero_interval", "FlushInterval: 0\n", 29535);
    REQUIRE_THROWS(ZestDB());
}

TEST_CASE("ZestDB archives do not contain previous archives", "[commands]") {
    TempDbDir tmp("zestdb_test_archive_nesting", "", 29537);
    tmp.writeConfig(std::format("DbPath: {}\nDBPort: 29537\nWebPort: 29538\nArchiveStoragePath: {}\nAutoArchiveSaving: false\n", tmp.dir.string(), (tmp.dir / "archive").string()));
    ZestDB db;
    db.execCmd(R"(s key "value")");
    REQUIRE(db.execCmd("save").code == ResultType::Code::Success);
    std::this_thread::sleep_for(std::chrono::milliseconds(1100));
    REQUIRE(db.execCmd("save").code == ResultType::Code::Success);

    for (const auto& file : fs::directory_iterator(tmp.dir / "archive")) {
        libzippp::ZipArchive zip(file.path().string());
        REQUIRE(zip.open(libzippp::ZipArchive::ReadOnly));
        for (const auto& entry : zip.getEntries()) {
            REQUIRE_FALSE(entry.getName().ends_with(".zip"));
        }
        zip.close();
    }
}

TEST_CASE_METHOD(ZestDBCmdFixture, "execCmd setby/delby with lim pick the smallest matching keys", "[commands]") {
    for (const char* k : { "k5", "k2", "k9", "k1", "k7", "k3" }) {
        db->set(k, "old");
    }
    REQUIRE(db->execCmd(R"(sb sw k "new" lim 3)").affectedRows == 3);
    for (const char* k : { "k1", "k2", "k3" }) {
        REQUIRE(db->get(k).response == "new");
    }
    for (const char* k : { "k5", "k7", "k9" }) {
        REQUIRE(db->get(k).response == "old");
    }
    REQUIRE(db->execCmd("db sw k lim 2").affectedRows == 2);
    REQUIRE(db->get("k1").code == ResultType::Code::KeyNotFound);
    REQUIRE(db->get("k2").code == ResultType::Code::KeyNotFound);
    REQUIRE(db->get("k3").response == "new");
}

TEST_CASE_METHOD(ZestDBCmdFixture, "execCmd set keeps trailing spaces but not the line ending", "[commands]") {
    db->execCmd("s key1 \"value  \"\r\n");
    REQUIRE(db->get("key1").response == "value  ");
}

TEST_CASE_METHOD(ZestDBCmdFixture, "execCmd scfg refuses negative numbers and zero intervals", "[commands]") {
    REQUIRE(db->execCmd("scfg cachesize -1").code == ResultType::Code::InvalidConfigValue);
    REQUIRE(db->execCmd("scfg segsize -1").code == ResultType::Code::InvalidConfigValue);
    REQUIRE(db->execCmd("scfg flushinterval 0").code == ResultType::Code::InvalidConfigValue);
    REQUIRE(db->execCmd("scfg archivecreationdelay 0").code == ResultType::Code::InvalidConfigValue);
    REQUIRE(db->execCmd("scfg compactinginterval 0").code == ResultType::Code::InvalidConfigValue);
}

TEST_CASE_METHOD(ZestDBCmdFixture, "responses stay valid JSON for values that are not UTF-8", "[commands]") {
    REQUIRE(db->execCmd(std::string("s k \"\xff\"")).code == ResultType::Code::Success);
    REQUIRE_NOTHROW(ZestDB::responseToJson(db->execCmd("g k")));
    REQUIRE_NOTHROW(ZestDB::responseToJson(db->execCmd("gb sw k")));
    REQUIRE_NOTHROW(ZestDB::responseToJson(db->execCmd(std::string("scfg \xff x"))));
}

TEST_CASE_METHOD(ZestDBCmdFixture, "execCmd scfg cannot toggle useSSL at runtime", "[commands]") {
    REQUIRE(db->execCmd("scfg usessl true").code == ResultType::Code::ConfigNotReloadable);
    REQUIRE(db->execCmd("scfg usessl false").code == ResultType::Code::ConfigNotReloadable);
}

TEST_CASE_METHOD(ZestDBCmdFixture, "execCmd refuses regexes that would blow up the regex compiler", "[commands]") {
    REQUIRE(db->execCmd("gb re " + std::string(50000, '(') + "a" + std::string(50000, ')')).code == ResultType::Code::InvalidRegex);
    REQUIRE(db->execCmd("gb re ((a{1000}){1000}){1000}").code == ResultType::Code::InvalidRegex);
    REQUIRE(db->execCmd("gb re a{50}b{50}").code == ResultType::Code::Success);
    REQUIRE(db->execCmd("gb re (ab|cd)+[a-z]{3}").code == ResultType::Code::Success);
}

TEST_CASE("ZestDB keeps only the configured number of archives", "[commands]") {
    TempDbDir tmp("zestdb_test_archive_retention", "ArchiveRetention: 2\n", 29541);
    ZestDB db;
    db.execCmd(R"(s key "value")");
    for (int i = 0; i < 3; i++) {
        REQUIRE(db.execCmd("save").code == ResultType::Code::Success);
    }
    size_t zips = 0;
    for (const auto& entry : fs::directory_iterator(tmp.dir / "archive")) {
        REQUIRE(entry.path().extension() == ".zip");
        zips++;
    }
    REQUIRE(zips == 2);
}

TEST_CASE("ZestDB keeps serving writes while an archive copies the files", "[commands]") {
    TempDbDir tmp("zestdb_test_archive_nonblocking", "SegSize: 1048576\nMaxValueSize: 100000\nfsyncOnWrite: false\n", 29561);
    ZestDB db;
    for (int i = 0; i < 640; i++) {
        REQUIRE(db.execCmd(std::format("s key{} \"{}\"", i, std::string(100000, static_cast<char>('a' + i % 26)))).code == ResultType::Code::Success);
    }

    const fs::path snapshot = tmp.dir / "archive" / ".snapshot";
    auto filesInSnapshot = [&snapshot] {
        size_t n = 0;
        std::error_code ec;
        for (auto it = fs::recursive_directory_iterator(snapshot, ec); !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
            if (it->is_regular_file(ec)) {
                n++;
            }
        }
        return n;
    };

    auto save = std::async(std::launch::async, [&db] { return db.execCmd("save").code; });
    while (filesInSnapshot() == 0 && save.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready) {
    }
    REQUIRE(db.execCmd(R"(s during "copy")").code == ResultType::Code::Success);
    const size_t copiedWhenWriteReturned = filesInSnapshot();
    REQUIRE(save.get() == ResultType::Code::Success);

    fs::path zipPath;
    for (const auto& entry : fs::directory_iterator(tmp.dir / "archive")) {
        zipPath = entry.path();
    }
    libzippp::ZipArchive zip(zipPath.string());
    REQUIRE(zip.open(libzippp::ZipArchive::ReadOnly));
    size_t archivedFiles = 0;
    for (const auto& entry : zip.getEntries()) {
        if (entry.isFile()) {
            archivedFiles++;
        }
    }
    REQUIRE(copiedWhenWriteReturned > 0);
    REQUIRE(copiedWhenWriteReturned < archivedFiles);
    zip.close();
}

TEST_CASE_METHOD(ZestDBCmdFixture, "execCmd setex stores a value with a TTL", "[commands][ttl]") {
    REQUIRE(db->execCmd(R"(setex k 60 "hello world")").code == ResultType::Code::Success);
    REQUIRE(db->get("k").response == "hello world");
    const int ttl = std::stoi(db->execCmd("ttl k").response);
    REQUIRE(ttl > 0);
    REQUIRE(ttl <= 60);

    REQUIRE(db->execCmd("persist k").code == ResultType::Code::Success);
    REQUIRE(db->execCmd("ttl k").response == "-1");
    REQUIRE(db->execCmd("ttl missing").response == "-2");
}

TEST_CASE_METHOD(ZestDBCmdFixture, "execCmd expired keys are invisible", "[commands][ttl]") {
    REQUIRE(db->execCmd(std::format("setexat gone {} \"value\"", nowMs() - 1)).code == ResultType::Code::Success);
    REQUIRE(db->execCmd(R"(s kept "value")").code == ResultType::Code::Success);

    REQUIRE(db->get("gone").code == ResultType::Code::KeyNotFound);
    REQUIRE(db->execCmd("ttl gone").response == "-2");
    REQUIRE(db->execCmd("d gone").code == ResultType::Code::KeyNotFound);
    REQUIRE(db->execCmd("expire gone 10").code == ResultType::Code::KeyNotFound);
    REQUIRE(db->execCmd("gb ew e").affectedRows == 0);
    REQUIRE(db->execCmd("gb ew t").affectedRows == 1);

    REQUIRE(db->execCmd(R"(s gone "back")").code == ResultType::Code::Success);
    REQUIRE(db->get("gone").response == "back");
}

TEST_CASE_METHOD(ZestDBCmdFixture, "execCmd expire makes a key disappear once its TTL is over", "[commands][ttl]") {
    db->execCmd(R"(s k "value")");
    REQUIRE(db->execCmd("expire k 1").code == ResultType::Code::Success);
    REQUIRE(db->get("k").response == "value");
    std::this_thread::sleep_for(std::chrono::milliseconds(1100));
    REQUIRE(db->get("k").code == ResultType::Code::KeyNotFound);
}

TEST_CASE_METHOD(ZestDBCmdFixture, "execCmd set removes the TTL of a key", "[commands][ttl]") {
    db->execCmd(R"(setex k 60 "old")");
    db->execCmd(R"(s k "new")");
    REQUIRE(db->execCmd("ttl k").response == "-1");
}

TEST_CASE_METHOD(ZestDBCmdFixture, "execCmd refuses invalid TTLs", "[commands][ttl]") {
    REQUIRE(db->execCmd(R"(setex k 0 "v")").code == ResultType::Code::InvalidTtl);
    REQUIRE(db->execCmd(R"(setex k -5 "v")").code == ResultType::Code::InvalidTtl);
    REQUIRE(db->execCmd(R"(setex k abc "v")").code == ResultType::Code::InvalidTtl);
    REQUIRE(db->execCmd("setex k 60").code == ResultType::Code::MissingValue);
    REQUIRE(db->execCmd("expire k").code == ResultType::Code::MissingArguments);
    REQUIRE(db->execCmd("expire missing 10").code == ResultType::Code::KeyNotFound);
}

TEST_CASE_METHOD(ZestDBCmdFixture, "execCmd TTL commands respect roles", "[commands][ttl]") {
    db->execCmd(R"(s k "v")");
    REQUIRE(db->execCmd(R"(setex k 60 "v")", Role::Read).code == ResultType::Code::PermissionDenied);
    REQUIRE(db->execCmd("expire k 60", Role::Read).code == ResultType::Code::PermissionDenied);
    REQUIRE(db->execCmd("persist k", Role::Read).code == ResultType::Code::PermissionDenied);
    REQUIRE(db->execCmd("ttl k", Role::Read).response == "-1");
}

TEST_CASE("ZestDB journals TTLs as absolute dates", "[commands][ttl]") {
    TempDbDir tmp("zestdb_test_ttl_wal", "", 29563);
    ZestDB db;
    REQUIRE(db.execCmd(R"(setex k 60 "hello world")").code == ResultType::Code::Success);
    REQUIRE(db.execCmd("expire k 120").code == ResultType::Code::Success);

    std::string wal;
    for (const auto& entry : fs::recursive_directory_iterator(tmp.dir / "db")) {
        if (entry.path().filename() == "WAL") {
            std::ifstream in(entry.path(), std::ios::binary);
            wal += std::string(std::istreambuf_iterator<char>(in), {});
        }
    }
    REQUIRE(wal.find("setexat k ") != std::string::npos);
    REQUIRE(wal.find(R"("hello world")") != std::string::npos);
    REQUIRE(wal.find("expireat k ") != std::string::npos);
    REQUIRE(wal.find("setex k") == std::string::npos);
}

TEST_CASE("ZestDB keeps TTLs across a restart", "[commands][ttl]") {
    TempDbDir tmp("zestdb_test_ttl_restart", "", 29565);
    {
        ZestDB db;
        db.execCmd(R"(setex k 60 "value")");
        db.execCmd(std::format("setexat gone {} \"value\"", nowMs() + 300));
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(400));
    ZestDB db;
    REQUIRE(db.get("k").response == "value");
    const int ttl = std::stoi(db.execCmd("ttl k").response);
    REQUIRE(ttl > 0);
    REQUIRE(ttl <= 60);
    REQUIRE(db.get("gone").code == ResultType::Code::KeyNotFound);
}

TEST_CASE_METHOD(ZestDBCmdFixture, "execCmd incr and decr update an integer value", "[commands][incr]") {
    REQUIRE(db->execCmd("incr n").response == "1");
    REQUIRE(db->execCmd("incr n 10").response == "11");
    REQUIRE(db->execCmd("incr n -3").response == "8");
    REQUIRE(db->execCmd("decr n").response == "7");
    REQUIRE(db->execCmd("decr n 10").response == "-3");
    REQUIRE(db->get("n").response == "-3");
    REQUIRE(db->execCmd("incr n").affectedRows == 1);

    db->execCmd(R"(s counter "41")");
    REQUIRE(db->execCmd("incr counter").response == "42");
}

TEST_CASE_METHOD(ZestDBCmdFixture, "execCmd incr refuses values and increments that are not integers", "[commands][incr]") {
    db->execCmd(R"(s name "Bob")");
    db->execCmd(R"(s spaced "5 ")");
    REQUIRE(db->execCmd("incr name").code == ResultType::Code::NotAnInteger);
    REQUIRE(db->execCmd("incr spaced").code == ResultType::Code::NotAnInteger);
    REQUIRE(db->get("name").response == "Bob");

    REQUIRE(db->execCmd("incr n abc").code == ResultType::Code::InvalidIncrement);
    REQUIRE(db->execCmd("incr n 1.5").code == ResultType::Code::InvalidIncrement);
    REQUIRE(db->execCmd("incr").code == ResultType::Code::MissingKey);
    REQUIRE(db->get("n").code == ResultType::Code::KeyNotFound);
}

TEST_CASE_METHOD(ZestDBCmdFixture, "execCmd incr reports overflows without changing the value", "[commands][incr]") {
    db->execCmd(std::format("s big \"{}\"", INT64_MAX));
    REQUIRE(db->execCmd("incr big").code == ResultType::Code::IntegerOverflow);
    REQUIRE(db->get("big").response == std::to_string(INT64_MAX));

    db->execCmd(std::format("s small \"{}\"", INT64_MIN));
    REQUIRE(db->execCmd("decr small").code == ResultType::Code::IntegerOverflow);
    REQUIRE(db->execCmd(std::format("decr n {}", INT64_MIN)).code == ResultType::Code::IntegerOverflow);
}

TEST_CASE_METHOD(ZestDBCmdFixture, "execCmd incr keeps the TTL and restarts expired keys at 0", "[commands][incr]") {
    db->execCmd(R"(setex n 60 "5")");
    REQUIRE(db->execCmd("incr n").response == "6");
    const int ttl = std::stoi(db->execCmd("ttl n").response);
    REQUIRE(ttl > 0);
    REQUIRE(ttl <= 60);

    db->execCmd(std::format("setexat gone {} \"100\"", nowMs() - 1));
    REQUIRE(db->execCmd("incr gone").response == "1");
    REQUIRE(db->execCmd("ttl gone").response == "-1");
}

TEST_CASE_METHOD(ZestDBCmdFixture, "execCmd incr respects roles and read-only mode", "[commands][incr]") {
    REQUIRE(db->execCmd("incr n", Role::Read).code == ResultType::Code::PermissionDenied);
    REQUIRE(db->execCmd("decr n", Role::Read).code == ResultType::Code::PermissionDenied);
    db->execCmd("scfg readonly true");
    REQUIRE(db->execCmd("incr n").code == ResultType::Code::ReadOnly);
}

TEST_CASE("ZestDB journals the result of incr, not the increment", "[commands][incr]") {
    TempDbDir tmp("zestdb_test_incr_wal", "", 29567);
    {
        ZestDB db;
        db.execCmd("incr n 5");
        db.execCmd(R"(setex t 60 "1")");
        db.execCmd("incr t");

        std::string wal;
        for (const auto& entry : fs::recursive_directory_iterator(tmp.dir / "db")) {
            if (entry.path().filename() == "WAL") {
                std::ifstream in(entry.path(), std::ios::binary);
                wal += std::string(std::istreambuf_iterator<char>(in), {});
            }
        }
        REQUIRE(wal.find(R"(s n "5")") != std::string::npos);
        REQUIRE(wal.find("setexat t ") != std::string::npos);
        REQUIRE(wal.find("incr") == std::string::npos);
    }
    ZestDB db;
    REQUIRE(db.get("n").response == "5");
    REQUIRE(db.get("t").response == "2");
}

TEST_CASE_METHOD(ZestDBCmdFixture, "execCmd setnx only writes keys that do not exist", "[commands][setnx]") {
    auto created = db->execCmd(R"(setnx lock "worker A")");
    REQUIRE(created.code == ResultType::Code::Success);
    REQUIRE(created.affectedRows == 1);
    REQUIRE(db->get("lock").response == "worker A");

    auto refused = db->execCmd(R"(setnx lock "worker B")");
    REQUIRE(refused.code == ResultType::Code::KeyExists);
    REQUIRE(refused.affectedRows == 0);
    REQUIRE(db->get("lock").response == "worker A");

    db->execCmd("d lock");
    REQUIRE(db->execCmd(R"(setnx lock "worker B")").code == ResultType::Code::Success);
    REQUIRE(db->get("lock").response == "worker B");
}

TEST_CASE_METHOD(ZestDBCmdFixture, "execCmd setnx treats an expired key as absent", "[commands][setnx]") {
    db->execCmd(std::format("setexat lock {} \"old\"", nowMs() - 1));
    REQUIRE(db->execCmd(R"(setnx lock "new")").code == ResultType::Code::Success);
    REQUIRE(db->get("lock").response == "new");
    REQUIRE(db->execCmd("ttl lock").response == "-1");
}

TEST_CASE_METHOD(ZestDBCmdFixture, "execCmd setnx validates like set", "[commands][setnx]") {
    REQUIRE(db->execCmd("setnx").code == ResultType::Code::MissingKey);
    REQUIRE(db->execCmd("setnx k").code == ResultType::Code::MissingValue);
    REQUIRE(db->execCmd(R"(setnx k "v")", Role::Read).code == ResultType::Code::PermissionDenied);
    db->execCmd("scfg jsononly true");
    REQUIRE(db->execCmd(R"(setnx k "notjson")").code == ResultType::Code::JsonOnly);
    db->execCmd("scfg jsononly false");
    db->execCmd("scfg readonly true");
    REQUIRE(db->execCmd(R"(setnx k "v")").code == ResultType::Code::ReadOnly);
    REQUIRE(db->get("k").code == ResultType::Code::KeyNotFound);
}

TEST_CASE("ZestDB journals a successful setnx as a set, and nothing when the key exists", "[commands][setnx]") {
    TempDbDir tmp("zestdb_test_setnx_wal", "", 29569);
    ZestDB db;
    db.execCmd(R"(setnx k "first")");
    db.execCmd(R"(setnx k "second")");

    std::string wal;
    for (const auto& entry : fs::recursive_directory_iterator(tmp.dir / "db")) {
        if (entry.path().filename() == "WAL") {
            std::ifstream in(entry.path(), std::ios::binary);
            wal += std::string(std::istreambuf_iterator<char>(in), {});
        }
    }
    REQUIRE(wal.find(R"(s k "first")") != std::string::npos);
    REQUIRE(wal.find("second") == std::string::npos);
    REQUIRE(wal.find("setnx") == std::string::npos);
}

TEST_CASE_METHOD(ZestDBCmdFixture, "execCmd cas replaces the value only when it matches", "[commands][cas]") {
    db->execCmd(R"(s balance "100")");
    auto swapped = db->execCmd(R"(cas balance "100" "80")");
    REQUIRE(swapped.code == ResultType::Code::Success);
    REQUIRE(swapped.affectedRows == 1);
    REQUIRE(db->get("balance").response == "80");

    REQUIRE(db->execCmd(R"(cas balance "100" "50")").code == ResultType::Code::ValueMismatch);
    REQUIRE(db->execCmd(R"(cas balance "8" "50")").code == ResultType::Code::ValueMismatch);
    REQUIRE(db->get("balance").response == "80");
}

TEST_CASE_METHOD(ZestDBCmdFixture, "execCmd cas handles spaces in both values", "[commands][cas]") {
    db->execCmd(R"(s user "{\"a\": 1}")");
    REQUIRE(db->execCmd(R"(cas user "{\"a\": 1}" "{\"a\": 2, \"b\": 3}")").code == ResultType::Code::Success);
    REQUIRE(db->get("user").response == R"({"a": 2, "b": 3})");
}

TEST_CASE_METHOD(ZestDBCmdFixture, "execCmd cas fails on missing or expired keys and keeps the TTL", "[commands][cas]") {
    REQUIRE(db->execCmd(R"(cas missing "a" "b")").code == ResultType::Code::KeyNotFound);
    db->execCmd(std::format("setexat gone {} \"a\"", nowMs() - 1));
    REQUIRE(db->execCmd(R"(cas gone "a" "b")").code == ResultType::Code::KeyNotFound);

    db->execCmd(R"(setex lock 60 "owner1")");
    REQUIRE(db->execCmd(R"(cas lock "owner1" "owner2")").code == ResultType::Code::Success);
    const int ttl = std::stoi(db->execCmd("ttl lock").response);
    REQUIRE(ttl > 0);
    REQUIRE(ttl <= 60);
}

TEST_CASE_METHOD(ZestDBCmdFixture, "execCmd cas rejects malformed values", "[commands][cas]") {
    db->execCmd(R"(s k "abc")");
    REQUIRE(db->execCmd("cas").code == ResultType::Code::MissingKey);
    REQUIRE(db->execCmd("cas k").code == ResultType::Code::MissingArguments);
    REQUIRE(db->execCmd("cas k abc new").code == ResultType::Code::InvalidValueFormat);
    REQUIRE(db->execCmd(R"(cas k "abc" new)").code == ResultType::Code::InvalidValueFormat);
    REQUIRE(db->execCmd(R"(cas k "abc" "new" extra)").code == ResultType::Code::InvalidValueFormat);
    REQUIRE(db->execCmd(R"(cas k "abc)").code == ResultType::Code::InvalidValueFormat);
    REQUIRE(db->execCmd(R"(cas k "abc")").code == ResultType::Code::MissingValue);
    REQUIRE(db->execCmd(R"(cas k "abc"   )").code == ResultType::Code::MissingValue);
    REQUIRE(db->execCmd(R"(cas k "abc" "new")", Role::Read).code == ResultType::Code::PermissionDenied);
    REQUIRE(db->get("k").response == "abc");
}

TEST_CASE("ZestDB journals a successful cas as a set", "[commands][cas]") {
    TempDbDir tmp("zestdb_test_cas_wal", "", 29571);
    ZestDB db;
    db.execCmd(R"(s k "old")");
    db.execCmd(R"(cas k "old" "new value")");
    db.execCmd(R"(cas k "old" "refused")");

    std::string wal;
    for (const auto& entry : fs::recursive_directory_iterator(tmp.dir / "db")) {
        if (entry.path().filename() == "WAL") {
            std::ifstream in(entry.path(), std::ios::binary);
            wal += std::string(std::istreambuf_iterator<char>(in), {});
        }
    }
    REQUIRE(wal.find(R"(s k "new value")") != std::string::npos);
    REQUIRE(wal.find("refused") == std::string::npos);
    REQUIRE(wal.find("cas") == std::string::npos);
}

TEST_CASE_METHOD(ZestDBCmdFixture, "execCmd values are quoted and unescape \\\" and \\\\ only", "[commands][quoting]") {
    REQUIRE(db->execCmd(R"(s k "say \"hi\" and \\ bye")").code == ResultType::Code::Success);
    REQUIRE(db->get("k").response == R"(say "hi" and \ bye)");

    REQUIRE(db->execCmd(R"(s k "line\nbreak")").code == ResultType::Code::Success);
    REQUIRE(db->get("k").response == R"(line\nbreak)");

    REQUIRE(db->execCmd(R"(s k "  padded  "   )").code == ResultType::Code::Success);
    REQUIRE(db->get("k").response == "  padded  ");
}

TEST_CASE_METHOD(ZestDBCmdFixture, "execCmd rejects values that are not properly quoted", "[commands][quoting]") {
    for (const char* cmd : { "s k value", R"(s k "unterminated)", R"(s k "a" b)", R"(s k "a\")", "setex k 60 value", "setnx k value", "sb sw k value", R"(sb sw k "v" lim)",
                             R"(sb sw k "v" extra)" }) {
        CAPTURE(cmd);
        REQUIRE(db->execCmd(cmd).code == ResultType::Code::InvalidValueFormat);
    }
    REQUIRE(db->execCmd(R"(s k "")").code == ResultType::Code::EmptyValue);
    REQUIRE(db->get("k").code == ResultType::Code::KeyNotFound);
}

TEST_CASE_METHOD(ZestDBCmdFixture, "execCmd setby reads lim only after the closing quote", "[commands][quoting]") {
    db->execCmd(R"(s k1 "a")");
    db->execCmd(R"(s k2 "b")");
    REQUIRE(db->execCmd(R"(sb sw k "paid lim 5")").affectedRows == 2);
    REQUIRE(db->get("k1").response == "paid lim 5");
    REQUIRE(db->execCmd(R"(sb sw k "x" lim 1)").affectedRows == 1);
    REQUIRE(db->execCmd(R"(sb sw k "x" LIM 1   )").affectedRows == 1);
}

TEST_CASE("ZestDB journals escaped values and replays them exactly", "[commands][quoting]") {
    TempDbDir tmp("zestdb_test_quoted_wal", "", 29573);
    {
        ZestDB db;
        REQUIRE(db.execCmd(R"(setex k 60 "a \"q\" \\ b")").code == ResultType::Code::Success);
        std::string wal;
        for (const auto& entry : fs::recursive_directory_iterator(tmp.dir / "db")) {
            if (entry.path().filename() == "WAL") {
                std::ifstream in(entry.path(), std::ios::binary);
                wal += std::string(std::istreambuf_iterator<char>(in), {});
            }
        }
        REQUIRE(wal.find(R"("a \"q\" \\ b")") != std::string::npos);
    }

    fs::create_directories(tmp.dir / "db" / "shards" / "0");
    {
        Settings s;
        s.fsyncOnWrite = false;
        WAL wal(s, tmp.dir / "db" / "shards" / "0" / "WAL");
        REQUIRE(wal.append(R"(s replayed "x \"y\" \\ z")"));
    }
    ZestDB db;
    REQUIRE(db.get("k").response == R"(a "q" \ b)");
    REQUIRE(db.get("replayed").response == R"(x "y" \ z)");
}

static std::string onlyArchive(const fs::path& archiveDir) {
    for (const auto& entry : fs::directory_iterator(archiveDir)) {
        if (entry.path().extension() == ".zip") {
            return entry.path().filename().string();
        }
    }
    return "";
}

static void writeZip(const fs::path& path, const std::vector<std::pair<std::string, std::string>>& entries) {
    libzippp::ZipArchive zip(path.string());
    REQUIRE(zip.open(libzippp::ZipArchive::New));
    for (const auto& [name, content] : entries) {
        REQUIRE(zip.addData(name, content.data(), content.size()));
    }
    REQUIRE(zip.close() == LIBZIPPP_OK);
}

TEST_CASE("ZestDB restore brings the database back to the archived state", "[commands][restore]") {
    TempDbDir tmp("zestdb_test_restore", "", 29575);
    std::string archive;
    {
        ZestDB db;
        db.execCmd(R"(s a "1")");
        db.execCmd(R"(s b "2")");
        db.execCmd(R"(setex t 60 "x")");
        REQUIRE(db.execCmd("save").code == ResultType::Code::Success);
        archive = onlyArchive(tmp.dir / "archive");
        REQUIRE_FALSE(archive.empty());

        db.execCmd(R"(s a "changed")");
        db.execCmd("d b");
        db.execCmd(R"(s c "new")");

        auto restored = db.execCmd("restore " + archive);
        REQUIRE(restored.code == ResultType::Code::Success);
        REQUIRE(db.get("a").response == "1");
        REQUIRE(db.get("b").response == "2");
        REQUIRE(db.get("c").code == ResultType::Code::KeyNotFound);
        const int ttl = std::stoi(db.execCmd("ttl t").response);
        REQUIRE(ttl > 0);
        REQUIRE(ttl <= 60);

        REQUIRE(db.execCmd(R"(s d "after")").code == ResultType::Code::Success);
        REQUIRE_FALSE(fs::exists(tmp.dir / "db" / ".restore"));
        REQUIRE_FALSE(fs::exists(tmp.dir / "db" / ".restore-previous"));
    }
    ZestDB reopened;
    REQUIRE(reopened.get("a").response == "1");
    REQUIRE(reopened.get("d").response == "after");
}

TEST_CASE("ZestDB restore only accepts archive names from ArchiveStoragePath", "[commands][restore]") {
    TempDbDir tmp("zestdb_test_restore_names", "", 29577);
    ZestDB db;
    db.execCmd(R"(s a "1")");
    REQUIRE(db.execCmd("save").code == ResultType::Code::Success);
    const std::string archive = onlyArchive(tmp.dir / "archive");
    fs::copy_file(tmp.dir / "archive" / archive, tmp.dir / "outside.zip");

    REQUIRE(db.execCmd("restore").code == ResultType::Code::MissingArguments);
    REQUIRE(db.execCmd("restore missing.zip").code == ResultType::Code::ArchiveNotFound);
    REQUIRE(db.execCmd("restore ../outside.zip").code == ResultType::Code::ArchiveNotFound);
    REQUIRE(db.execCmd("restore " + (tmp.dir / "outside.zip").string()).code == ResultType::Code::ArchiveNotFound);
    REQUIRE(db.execCmd("restore " + archive, Role::Write).code == ResultType::Code::PermissionDenied);
    REQUIRE(db.get("a").response == "1");
}

TEST_CASE("ZestDB restore refuses invalid archives and keeps the data", "[commands][restore]") {
    TempDbDir tmp("zestdb_test_restore_invalid", "", 29579);
    ZestDB db;
    db.execCmd(R"(s a "1")");
    fs::create_directories(tmp.dir / "archive");

    std::ofstream(tmp.dir / "archive" / "garbage.zip") << "not a zip file";
    REQUIRE(db.execCmd("restore garbage.zip").code == ResultType::Code::InvalidArchive);

    writeZip(tmp.dir / "archive" / "empty.zip", { { "notes.txt", "no shards here" } });
    REQUIRE(db.execCmd("restore empty.zip").code == ResultType::Code::InvalidArchive);

    writeZip(tmp.dir / "archive" / "slip.zip", { { "shards/0/WAL", "" }, { "../../evil.txt", "escaped" } });
    REQUIRE(db.execCmd("restore slip.zip").code == ResultType::Code::InvalidArchive);
    REQUIRE_FALSE(fs::exists(tmp.dir / "evil.txt"));
    REQUIRE_FALSE(fs::exists(tmp.dir.parent_path() / "evil.txt"));

    REQUIRE(db.get("a").response == "1");
    REQUIRE(db.execCmd(R"(s b "2")").code == ResultType::Code::Success);
}

TEST_CASE("ZestDB lsaves lists the archives oldest first", "[commands][restore]") {
    TempDbDir tmp("zestdb_test_lsaves", "", 29581);
    ZestDB db;

    auto empty = db.execCmd("lsaves");
    REQUIRE(empty.code == ResultType::Code::Success);
    REQUIRE(empty.response == "[]");
    REQUIRE(empty.affectedRows == 0);

    db.execCmd(R"(s a "1")");
    REQUIRE(db.execCmd("save").code == ResultType::Code::Success);
    std::this_thread::sleep_for(std::chrono::milliseconds(1100));
    db.execCmd(R"(s a "2")");
    REQUIRE(db.execCmd("save").code == ResultType::Code::Success);
    std::ofstream(tmp.dir / "archive" / "notes.txt") << "not an archive";

    auto listed = db.execCmd("lsaves");
    REQUIRE(listed.affectedRows == 2);
    const auto names = nlohmann::json::parse(listed.response).get<std::vector<std::string>>();
    REQUIRE(names.size() == 2);
    REQUIRE(names[0] < names[1]);
    REQUIRE(names[0].starts_with("zestdb_archive_"));

    REQUIRE(db.execCmd("restore " + names[0]).code == ResultType::Code::Success);
    REQUIRE(db.get("a").response == "1");

    REQUIRE(db.execCmd("lsaves", Role::Write).code == ResultType::Code::PermissionDenied);
}

TEST_CASE_METHOD(ZestDBCmdFixture, "execCmd exists tells whether a key is live", "[commands][exists]") {
    auto missing = db->execCmd("exists k");
    REQUIRE(missing.code == ResultType::Code::Success);
    REQUIRE(missing.response == "false");
    REQUIRE(missing.affectedRows == 0);

    db->execCmd(R"(s k "v")");
    auto present = db->execCmd("exists k");
    REQUIRE(present.response == "true");
    REQUIRE(present.affectedRows == 1);

    db->execCmd("d k");
    REQUIRE(db->execCmd("exists k").response == "false");

    db->execCmd(std::format(R"(setexat gone {} "v")", nowMs() - 1));
    REQUIRE(db->execCmd("exists gone").response == "false");
    db->execCmd(R"(setex later 60 "v")");
    REQUIRE(db->execCmd("exists later").response == "true");
}

TEST_CASE_METHOD(ZestDBCmdFixture, "execCmd exists validates the key and is allowed to readers", "[commands][exists]") {
    REQUIRE(db->execCmd("exists").code == ResultType::Code::MissingKey);
    REQUIRE(db->execCmd("exists " + std::string(300, 'k')).code == ResultType::Code::InvalidKey);

    db->execCmd(R"(s k "v")");
    REQUIRE(db->execCmd("exists k", Role::Read).response == "true");
}

TEST_CASE("ZestDB exists finds keys after a restart", "[commands][exists]") {
    TempDbDir tmp("zestdb_test_exists_restart", "CacheSize: 0\n", 29583);
    {
        ZestDB db;
        db.execCmd(R"(s k "v")");
    }
    ZestDB db;
    REQUIRE(db.execCmd("exists k").response == "true");
    REQUIRE(db.execCmd("exists other").response == "false");
}

TEST_CASE_METHOD(ZestDBCmdFixture, "execCmd count counts live keys, optionally by pattern", "[commands][count]") {
    auto empty = db->execCmd("count");
    REQUIRE(empty.code == ResultType::Code::Success);
    REQUIRE(empty.response == "0");
    REQUIRE(empty.affectedRows == 0);

    for (int i = 0; i < 50; i++) {
        db->execCmd(std::format(R"(s user:{} "u")", i));
        db->execCmd(std::format(R"(s order:{} "o")", i));
    }
    auto all = db->execCmd("count");
    REQUIRE(all.response == "100");
    REQUIRE(all.affectedRows == 100);

    REQUIRE(db->execCmd("count sw user:").response == "50");
    REQUIRE(db->execCmd("count SW user:").response == "50");
    REQUIRE(db->execCmd("count ew :7").response == "2");
    REQUIRE(db->execCmd("count ct :4").response == "22");
    REQUIRE(db->execCmd(R"(count re order:\d)").response == "10");
    REQUIRE(db->execCmd("count sw nothing").response == "0");

    db->execCmd("db sw order:");
    db->execCmd(std::format(R"(setexat user:0 {} "u")", nowMs() - 1));
    REQUIRE(db->execCmd("count").response == "49");
}

TEST_CASE_METHOD(ZestDBCmdFixture, "execCmd count rejects incomplete or invalid patterns", "[commands][count]") {
    REQUIRE(db->execCmd("count sw").code == ResultType::Code::MissingPattern);
    REQUIRE(db->execCmd("count xx abc").code == ResultType::Code::InvalidRegex);
    REQUIRE(db->execCmd(R"(count re (a)\1)").code == ResultType::Code::InvalidRegex);
    REQUIRE(db->execCmd("count", Role::Read).code == ResultType::Code::Success);
}

TEST_CASE("ZestDB stats server applies NetworkValidation", "[commands]") {
    TempDbDir tmp("zestdb_test_http_filter", "NetworkValidation: '^10\\.'\n", 29543);
    ZestDB db;
    std::thread http([&db] { db.srv->listen("127.0.0.1", 29544); });
    db.srv->wait_until_ready();
    httplib::Client client("127.0.0.1", 29544);
    auto res = client.Get("/");
    db.srv->stop();
    http.join();
    REQUIRE(res);
    REQUIRE(res->status == 403);
}
