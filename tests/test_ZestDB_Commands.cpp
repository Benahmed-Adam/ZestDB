#include <catch2/catch_all.hpp>
#include <chrono>
#include <filesystem>
#include <format>
#include <fstream>
#include <future>
#include <libzippp/libzippp.h>
#include <thread>

#include "ZestDB.hpp"
#include "lib/json.hpp"
#include "WAL.hpp"

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
    REQUIRE(r.code == ResultType::Code::Error);
}

TEST_CASE_METHOD(ZestDBCmdFixture, "execCmd whitespace only", "[commands]") {
    auto r = db->execCmd("   \t  ");
    REQUIRE(r.code == ResultType::Code::Error);
}

TEST_CASE_METHOD(ZestDBCmdFixture, "execCmd unknown command", "[commands]") {
    auto r = db->execCmd("xyz");
    REQUIRE(r.response.find("Command not found") != std::string::npos);
}

TEST_CASE_METHOD(ZestDBCmdFixture, "execCmd help", "[commands]") {
    auto r = db->execCmd("h");
    REQUIRE(r.code == ResultType::Code::Success);
}

TEST_CASE_METHOD(ZestDBCmdFixture, "execCmd set and get", "[commands]") {
    auto s = db->execCmd("s key1 value1");
    REQUIRE(s.code == ResultType::Code::Success);
    auto g = db->execCmd("g key1");
    REQUIRE(g.code == ResultType::Code::Success);
}

TEST_CASE_METHOD(ZestDBCmdFixture, "execCmd get missing key", "[commands]") {
    auto r = db->execCmd("g");
    REQUIRE(r.response.find("missing key") != std::string::npos);
}

TEST_CASE_METHOD(ZestDBCmdFixture, "execCmd set missing key", "[commands]") {
    auto r = db->execCmd("s");
    REQUIRE(r.response.find("missing key") != std::string::npos);
}

TEST_CASE_METHOD(ZestDBCmdFixture, "execCmd set missing value", "[commands]") {
    auto r = db->execCmd("s key1");
    REQUIRE(r.response.find("missing value") != std::string::npos);
}

TEST_CASE_METHOD(ZestDBCmdFixture, "execCmd set preserves spaces in value", "[commands]") {
    db->execCmd("s key1 hello world foo");
    auto g = db->execCmd("g key1");
    REQUIRE(g.response == "hello world foo");
}

TEST_CASE_METHOD(ZestDBCmdFixture, "execCmd del existing key", "[commands]") {
    db->execCmd("s key1 val");
    auto r = db->execCmd("d key1");
    REQUIRE(r.code == ResultType::Code::Success);
}

TEST_CASE_METHOD(ZestDBCmdFixture, "execCmd del missing key", "[commands]") {
    auto r = db->execCmd("d");
    REQUIRE(r.response.find("missing key") != std::string::npos);
}

TEST_CASE_METHOD(ZestDBCmdFixture, "execCmd getby missing args", "[commands]") {
    auto r = db->execCmd("gb");
    REQUIRE(r.response.find("Not enough arguments") != std::string::npos);
}

TEST_CASE_METHOD(ZestDBCmdFixture, "execCmd getby valid re", "[commands]") {
    db->execCmd("s test_1 v1");
    db->execCmd("s test_2 v2");
    auto r = db->execCmd("gb re test_.*");
    REQUIRE(r.code == ResultType::Code::Success);
    REQUIRE(r.affectedRows == 2);
}

TEST_CASE_METHOD(ZestDBCmdFixture, "execCmd getby invalid mode", "[commands]") {
    auto r = db->execCmd("gb xx pat");
    REQUIRE(r.response.find("Invalid search mode") != std::string::npos);
}

TEST_CASE_METHOD(ZestDBCmdFixture, "execCmd getby with lim", "[commands]") {
    db->execCmd("s a1 v1");
    db->execCmd("s a2 v2");
    db->execCmd("s a3 v3");
    auto r = db->execCmd("gb sw a lim 2");
    REQUIRE(r.code == ResultType::Code::Success);
    REQUIRE(r.affectedRows == 2);
}

TEST_CASE_METHOD(ZestDBCmdFixture, "execCmd setby missing value", "[commands]") {
    auto r = db->execCmd("sb re .*");
    REQUIRE(r.response.find("missing value") != std::string::npos);
}

TEST_CASE_METHOD(ZestDBCmdFixture, "execCmd setby valid", "[commands]") {
    db->execCmd("s abc_1 old");
    db->execCmd("s abc_2 old");
    auto r = db->execCmd("sb sw abc_ newval");
    REQUIRE(r.code == ResultType::Code::Success);
}

TEST_CASE_METHOD(ZestDBCmdFixture, "execCmd setby preserves spaces in value", "[commands]") {
    db->execCmd("s abc_1 old");
    db->execCmd("sb sw abc_ hello world");
    auto g = db->execCmd("g abc_1");
    REQUIRE(g.response == "hello world");
}

TEST_CASE_METHOD(ZestDBCmdFixture, "execCmd delby missing pattern", "[commands]") {
    auto r = db->execCmd("db");
    REQUIRE(r.response.find("missing pattern") != std::string::npos);
}

TEST_CASE_METHOD(ZestDBCmdFixture, "execCmd delby valid", "[commands]") {
    db->execCmd("s abc_1 v1");
    db->execCmd("s abc_2 v2");
    db->execCmd("s xyz_1 v3");
    auto r = db->execCmd("db sw abc_");
    REQUIRE(r.code == ResultType::Code::Success);
    REQUIRE(r.affectedRows == 2);
}

TEST_CASE_METHOD(ZestDBCmdFixture, "execCmd delby invalid mode", "[commands]") {
    auto r = db->execCmd("db xx pat");
    REQUIRE(r.response.find("Invalid mode") != std::string::npos);
}

TEST_CASE_METHOD(ZestDBCmdFixture, "execCmd case insensitive", "[commands]") {
    auto r = db->execCmd("S key1 value1");
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
    REQUIRE(r.response.find("Flush successful") != std::string::npos);
}

TEST_CASE_METHOD(ZestDBCmdFixture, "execCmd scfg missing args", "[commands]") {
    auto r = db->execCmd("scfg");
    REQUIRE(r.response.find("Not enough arguments") != std::string::npos);
}

TEST_CASE_METHOD(ZestDBCmdFixture, "execCmd scfg segsize valid", "[commands]") {
    auto r = db->execCmd("scfg segsize 200000");
    REQUIRE(r.code == ResultType::Code::Success);
}

TEST_CASE_METHOD(ZestDBCmdFixture, "execCmd scfg segsize too small", "[commands]") {
    auto r = db->execCmd("scfg segsize 5");
    REQUIRE(r.response.find("SegSize must be greater") != std::string::npos);
}

TEST_CASE_METHOD(ZestDBCmdFixture, "execCmd scfg maxkeysize valid", "[commands]") {
    auto r = db->execCmd("scfg maxkeysize 128");
    REQUIRE(r.code == ResultType::Code::Success);
}

TEST_CASE_METHOD(ZestDBCmdFixture, "execCmd scfg maxkeysize too large", "[commands]") {
    auto r = db->execCmd("scfg maxkeysize 300");
    REQUIRE(r.response.find("cannot exceed") != std::string::npos);
}

TEST_CASE_METHOD(ZestDBCmdFixture, "execCmd scfg maxvaluesize valid", "[commands]") {
    auto r = db->execCmd("scfg maxvaluesize 5000");
    REQUIRE(r.code == ResultType::Code::Success);
}

TEST_CASE_METHOD(ZestDBCmdFixture, "execCmd scfg maxvaluesize too large", "[commands]") {
    auto r = db->execCmd("scfg maxvaluesize 999999");
    REQUIRE(r.response.find("MaxValueSize must be less") != std::string::npos);
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
    REQUIRE(r.response.find("Invalid isDebug") != std::string::npos);
}

TEST_CASE_METHOD(ZestDBCmdFixture, "execCmd scfg unknown param", "[commands]") {
    auto r = db->execCmd("scfg unknown 123");
    REQUIRE(r.response.find("Unknown parameter") != std::string::npos);
}

TEST_CASE_METHOD(ZestDBCmdFixture, "execCmd scfg non-numeric int", "[commands]") {
    auto r = db->execCmd("scfg segsize abc");
    REQUIRE(r.response.find("Invalid") != std::string::npos);
}

TEST_CASE_METHOD(ZestDBCmdFixture, "execCmd delby with lim", "[commands]") {
    db->execCmd("s a1 v1");
    db->execCmd("s a2 v2");
    db->execCmd("s a3 v3");
    auto r = db->execCmd("db sw a lim 1");
    REQUIRE(r.code == ResultType::Code::Success);
    REQUIRE(r.affectedRows == 1);
}

TEST_CASE_METHOD(ZestDBCmdFixture, "execCmd readonly blocks set", "[commands]") {
    db->execCmd("scfg readonly true");
    auto r = db->execCmd("s key1 val");
    REQUIRE(r.response.find("read-only") != std::string::npos);
}

TEST_CASE_METHOD(ZestDBCmdFixture, "execCmd readonly blocks del", "[commands]") {
    db->execCmd("scfg readonly true");
    auto r = db->execCmd("d key1");
    REQUIRE(r.response.find("read-only") != std::string::npos);
}

TEST_CASE_METHOD(ZestDBCmdFixture, "execCmd readonly allows get", "[commands]") {
    db->execCmd("scfg readonly true");
    auto r = db->execCmd("g key1");
    REQUIRE(r.code == ResultType::Code::Error);
    REQUIRE(r.response.find("read-only") == std::string::npos);
}

TEST_CASE_METHOD(ZestDBCmdFixture, "execCmd jsononly blocks non-json", "[commands]") {
    db->execCmd("scfg jsononly true");
    auto r = db->execCmd("s key1 plainvalue");
    REQUIRE(r.response.find("json") != std::string::npos);
}

TEST_CASE_METHOD(ZestDBCmdFixture, "execCmd jsononly allows json", "[commands]") {
    db->execCmd("scfg jsononly true");
    auto r = db->execCmd("s key1 {\"a\":1}");
    REQUIRE(r.code == ResultType::Code::Success);
}

TEST_CASE_METHOD(ZestDBCmdFixture, "execCmd getby lim counts matches, not scanned keys", "[commands]") {
    db->set("target", "x");
    for (int i = 0; i < 500; i++) {
        db->set("k" + std::to_string(i), "v");
    }
    REQUIRE(db->execCmd("gb sw target lim 1").affectedRows == 1);
    REQUIRE(db->execCmd("sb sw target y lim 1").affectedRows == 1);
    REQUIRE(db->execCmd("db sw target lim 1").affectedRows == 1);
}

TEST_CASE_METHOD(ZestDBCmdFixture, "execCmd invalid lim is an error, not an exception", "[commands]") {
    for (const char* cmd : { "gb sw a lim abc", "sb sw a val lim x", "db sw a lim -1" }) {
        ResultType r;
        REQUIRE_NOTHROW(r = db->execCmd(cmd));
        REQUIRE(r.code == ResultType::Code::Error);
        REQUIRE(r.response == Messages::INVALID_LIMIT);
    }
}

TEST_CASE_METHOD(ZestDBCmdFixture, "execCmd set key that appears earlier in the command", "[commands]") {
    db->execCmd("s s hello");
    REQUIRE(db->execCmd("g s").response == "hello");
}

TEST_CASE_METHOD(ZestDBCmdFixture, "execCmd setby value equal to pattern", "[commands]") {
    db->execCmd("s user1 a");
    db->execCmd("sb sw user user");
    REQUIRE(db->execCmd("g user1").response == "user");
}

TEST_CASE_METHOD(ZestDBCmdFixture, "execCmd accepts long command names", "[commands]") {
    REQUIRE(db->execCmd("set key1 v1").code == ResultType::Code::Success);
    REQUIRE(db->execCmd("GET key1").response == "v1");
    REQUIRE(db->execCmd("getby sw key").affectedRows == 1);
    REQUIRE(db->execCmd("setby sw key v2").affectedRows == 1);
    REQUIRE(db->execCmd("delby sw key").affectedRows == 1);
    REQUIRE(db->execCmd("del key1").code == ResultType::Code::Error);
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
    REQUIRE(db->execCmd("sb sw k hello world lim 2").affectedRows == 2);
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
    REQUIRE(db->execCmd(R"(gb re (a)\1)").response == Messages::INVALID_REGEX);
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
        REQUIRE(wal.append("s replayed a value longer than five"));
    }
    ZestDB db;
    REQUIRE(db.get("replayed").response == "a value longer than five");
}

TEST_CASE("ZestDB reload with a broken config keeps the users", "[commands]") {
    TempDbDir tmp("zestdb_test_reload_users", "users:\n  - user: alice\n    password: secret\n", 29533);
    ZestDB db;
    REQUIRE(db.validateToken("alice", sha256("alicesecret")));
    tmp.writeConfig("users: [\n  - user: {\n");
    REQUIRE(db.execCmd("r").code == ResultType::Code::Error);
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
    db.execCmd("s key value");
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
    REQUIRE(db->execCmd("sb sw k new lim 3").affectedRows == 3);
    for (const char* k : { "k1", "k2", "k3" }) {
        REQUIRE(db->get(k).response == "new");
    }
    for (const char* k : { "k5", "k7", "k9" }) {
        REQUIRE(db->get(k).response == "old");
    }
    REQUIRE(db->execCmd("db sw k lim 2").affectedRows == 2);
    REQUIRE(db->get("k1").code == ResultType::Code::Error);
    REQUIRE(db->get("k2").code == ResultType::Code::Error);
    REQUIRE(db->get("k3").response == "new");
}

TEST_CASE_METHOD(ZestDBCmdFixture, "execCmd set keeps trailing spaces but not the line ending", "[commands]") {
    db->execCmd("s key1 value  \r\n");
    REQUIRE(db->get("key1").response == "value  ");
}

TEST_CASE_METHOD(ZestDBCmdFixture, "execCmd scfg refuses negative numbers and zero intervals", "[commands]") {
    REQUIRE(db->execCmd("scfg cachesize -1").code == ResultType::Code::Error);
    REQUIRE(db->execCmd("scfg segsize -1").code == ResultType::Code::Error);
    REQUIRE(db->execCmd("scfg flushinterval 0").code == ResultType::Code::Error);
    REQUIRE(db->execCmd("scfg archivecreationdelay 0").code == ResultType::Code::Error);
    REQUIRE(db->execCmd("scfg compactinginterval 0").code == ResultType::Code::Error);
}

TEST_CASE_METHOD(ZestDBCmdFixture, "responses stay valid JSON for values that are not UTF-8", "[commands]") {
    REQUIRE(db->execCmd(std::string("s k \xff")).code == ResultType::Code::Success);
    REQUIRE_NOTHROW(ZestDB::responseToJson(db->execCmd("g k")));
    REQUIRE_NOTHROW(ZestDB::responseToJson(db->execCmd("gb sw k")));
    REQUIRE_NOTHROW(ZestDB::responseToJson(db->execCmd(std::string("scfg \xff x"))));
}

TEST_CASE_METHOD(ZestDBCmdFixture, "execCmd scfg cannot toggle useSSL at runtime", "[commands]") {
    REQUIRE(db->execCmd("scfg usessl true").code == ResultType::Code::Error);
    REQUIRE(db->execCmd("scfg usessl false").code == ResultType::Code::Error);
}

TEST_CASE_METHOD(ZestDBCmdFixture, "execCmd refuses regexes that would blow up the regex compiler", "[commands]") {
    REQUIRE(db->execCmd("gb re " + std::string(50000, '(') + "a" + std::string(50000, ')')).response == Messages::INVALID_REGEX);
    REQUIRE(db->execCmd("gb re ((a{1000}){1000}){1000}").response == Messages::INVALID_REGEX);
    REQUIRE(db->execCmd("gb re a{50}b{50}").code == ResultType::Code::Success);
    REQUIRE(db->execCmd("gb re (ab|cd)+[a-z]{3}").code == ResultType::Code::Success);
}

TEST_CASE("ZestDB keeps only the configured number of archives", "[commands]") {
    TempDbDir tmp("zestdb_test_archive_retention", "ArchiveRetention: 2\n", 29541);
    ZestDB db;
    db.execCmd("s key value");
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
        REQUIRE(db.execCmd(std::format("s key{} {}", i, std::string(100000, static_cast<char>('a' + i % 26)))).code == ResultType::Code::Success);
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
    REQUIRE(db.execCmd("s during copy").code == ResultType::Code::Success);
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
