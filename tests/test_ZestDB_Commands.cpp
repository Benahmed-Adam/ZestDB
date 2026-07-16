#include <catch2/catch_all.hpp>
#include <filesystem>
#include <fstream>

#include "ZestDB.hpp"

using namespace Zest;
namespace fs = std::filesystem;

class ZestDBCmdFixture {
public:
    fs::path testDir;
    fs::path originalCwd;
    ZestDB *db = nullptr;

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
    REQUIRE(r.code == ResultType::Code::ERROR);
}

TEST_CASE_METHOD(ZestDBCmdFixture, "execCmd whitespace only", "[commands]") {
    auto r = db->execCmd("   \t  ");
    REQUIRE(r.code == ResultType::Code::ERROR);
}

TEST_CASE_METHOD(ZestDBCmdFixture, "execCmd unknown command", "[commands]") {
    auto r = db->execCmd("xyz");
    REQUIRE(r.response.find("Command not found") != std::string::npos);
}

TEST_CASE_METHOD(ZestDBCmdFixture, "execCmd help", "[commands]") {
    auto r = db->execCmd("h");
    REQUIRE(r.code == ResultType::Code::SUCCESS);
}

TEST_CASE_METHOD(ZestDBCmdFixture, "execCmd set and get", "[commands]") {
    auto s = db->execCmd("s key1 value1");
    REQUIRE(s.code == ResultType::Code::SUCCESS);
    auto g = db->execCmd("g key1");
    REQUIRE(g.code == ResultType::Code::SUCCESS);
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
    REQUIRE(r.code == ResultType::Code::SUCCESS);
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
    REQUIRE(r.code == ResultType::Code::SUCCESS);
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
    REQUIRE(r.code == ResultType::Code::SUCCESS);
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
    REQUIRE(r.code == ResultType::Code::SUCCESS);
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
    REQUIRE(r.code == ResultType::Code::SUCCESS);
    REQUIRE(r.affectedRows == 2);
}

TEST_CASE_METHOD(ZestDBCmdFixture, "execCmd delby invalid mode", "[commands]") {
    auto r = db->execCmd("db xx pat");
    REQUIRE(r.response.find("Invalid mode") != std::string::npos);
}

TEST_CASE_METHOD(ZestDBCmdFixture, "execCmd case insensitive", "[commands]") {
    auto r = db->execCmd("S key1 value1");
    REQUIRE(r.code == ResultType::Code::SUCCESS);
    auto g = db->execCmd("G key1");
    REQUIRE(g.code == ResultType::Code::SUCCESS);
}

TEST_CASE_METHOD(ZestDBCmdFixture, "execCmd getconfig", "[commands]") {
    auto r = db->execCmd("gcfg");
    REQUIRE(r.code == ResultType::Code::SUCCESS);
}

TEST_CASE_METHOD(ZestDBCmdFixture, "execCmd flush", "[commands]") {
    auto r = db->execCmd("f");
    REQUIRE(r.code == ResultType::Code::SUCCESS);
    REQUIRE(r.response.find("Flush successful") != std::string::npos);
}

TEST_CASE_METHOD(ZestDBCmdFixture, "execCmd scfg missing args", "[commands]") {
    auto r = db->execCmd("scfg");
    REQUIRE(r.response.find("Not enough arguments") != std::string::npos);
}

TEST_CASE_METHOD(ZestDBCmdFixture, "execCmd scfg segsize valid", "[commands]") {
    auto r = db->execCmd("scfg segsize 200000");
    REQUIRE(r.code == ResultType::Code::SUCCESS);
}

TEST_CASE_METHOD(ZestDBCmdFixture, "execCmd scfg segsize too small", "[commands]") {
    auto r = db->execCmd("scfg segsize 5");
    REQUIRE(r.response.find("SegSize must be greater") != std::string::npos);
}

TEST_CASE_METHOD(ZestDBCmdFixture, "execCmd scfg maxkeysize valid", "[commands]") {
    auto r = db->execCmd("scfg maxkeysize 128");
    REQUIRE(r.code == ResultType::Code::SUCCESS);
}

TEST_CASE_METHOD(ZestDBCmdFixture, "execCmd scfg maxkeysize too large", "[commands]") {
    auto r = db->execCmd("scfg maxkeysize 300");
    REQUIRE(r.response.find("cannot exceed") != std::string::npos);
}

TEST_CASE_METHOD(ZestDBCmdFixture, "execCmd scfg maxvaluesize valid", "[commands]") {
    auto r = db->execCmd("scfg maxvaluesize 5000");
    REQUIRE(r.code == ResultType::Code::SUCCESS);
}

TEST_CASE_METHOD(ZestDBCmdFixture, "execCmd scfg maxvaluesize too large", "[commands]") {
    auto r = db->execCmd("scfg maxvaluesize 999999");
    REQUIRE(r.response.find("MaxValueSize must be less") != std::string::npos);
}

TEST_CASE_METHOD(ZestDBCmdFixture, "execCmd scfg cachesize", "[commands]") {
    auto r = db->execCmd("scfg cachesize 5000");
    REQUIRE(r.code == ResultType::Code::SUCCESS);
}

TEST_CASE_METHOD(ZestDBCmdFixture, "execCmd scfg isdebug true", "[commands]") {
    auto r = db->execCmd("scfg isdebug true");
    REQUIRE(r.code == ResultType::Code::SUCCESS);
}

TEST_CASE_METHOD(ZestDBCmdFixture, "execCmd scfg isdebug false", "[commands]") {
    auto r = db->execCmd("scfg isdebug 0");
    REQUIRE(r.code == ResultType::Code::SUCCESS);
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
    REQUIRE(r.code == ResultType::Code::SUCCESS);
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
    REQUIRE(r.code == ResultType::Code::ERROR);
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
    REQUIRE(r.code == ResultType::Code::SUCCESS);
}
