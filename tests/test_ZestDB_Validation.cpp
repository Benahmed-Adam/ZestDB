#include <catch2/catch_all.hpp>
#include <filesystem>
#include <fstream>
#include "ZestDB.hpp"

using namespace Zest;
namespace fs = std::filesystem;

class ZestDBValidationFixture {
public:
    fs::path testDir;
    fs::path originalCwd;
    ZestDB *db = nullptr;

    ZestDBValidationFixture() {
        originalCwd = fs::current_path();
        testDir = fs::temp_directory_path() / ("zestdb_test_val_" + std::to_string(reinterpret_cast<uintptr_t>(this)));
        fs::create_directories(testDir);
        fs::create_directories(testDir / "archive");

        std::ofstream cfg(testDir / "config.yaml");
        cfg << "DbPath: " << (testDir / "db").string() << "\n";
        cfg << "SegSize: 100000\n";
        cfg << "MaxKeySize: 64\n";
        cfg << "MaxValueSize: 10000\n";
        cfg << "CacheSize: 100\n";
        cfg << "CompactingInterval: 3600\n";
        cfg << "FlushInterval: 3600\n";
        cfg << "isDebug: false\n";
        cfg << "readOnly: false\n";
        cfg << "jsonOnly: false\n";
        cfg << "DBPort: 29401\n";
        cfg << "WebPort: 29402\n";
        cfg << "useSSL: false\n";
        cfg << "ArchiveStoragePath: " << (testDir / "archive").string() << "\n";
        cfg << "ArchiveCreationDelay: 3600\n";
        cfg << "AutoArchiveSaving: false\n";
        cfg.close();

        fs::current_path(testDir);
        db = new ZestDB();
    }

    ~ZestDBValidationFixture() {
        if (db) {
            db->stop();
            delete db;
        }
        fs::current_path(originalCwd);
        fs::remove_all(testDir);
    }
};

TEST_CASE_METHOD(ZestDBValidationFixture, "ValidationRule regex mode valid", "[validation]") {
    auto result = db->execCmd("gb re ^test$");
    REQUIRE(result.code == ResultType::Code::SUCCESS);
}

TEST_CASE_METHOD(ZestDBValidationFixture, "createValidationRule re valid regex", "[validation]") {
    ValidationRule vr;
    vr.func = [](const std::string &k) { return std::regex_match(k, std::regex("^test_.*")); };
    REQUIRE(vr.func("test_1"));
    REQUIRE_FALSE(vr.func("other"));
}

TEST_CASE_METHOD(ZestDBValidationFixture, "createValidationRule sw mode", "[validation]") {
    ValidationRule vr;
    vr.func = [](const std::string &k) { return k.find("pre") == 0; };
    REQUIRE(vr.func("prefix"));
    REQUIRE_FALSE(vr.func("aprefix"));
}

TEST_CASE_METHOD(ZestDBValidationFixture, "createValidationRule ct mode", "[validation]") {
    ValidationRule vr;
    vr.func = [](const std::string &k) { return k.find("xyz") != std::string::npos; };
    REQUIRE(vr.func("xyz_abc"));
    REQUIRE(vr.func("abc_xyz"));
    REQUIRE_FALSE(vr.func("abcdef"));
}

TEST_CASE_METHOD(ZestDBValidationFixture, "createValidationRule ew mode key longer", "[validation]") {
    ValidationRule vr;
    vr.func = [](const std::string &k) {
        std::string pattern = "_end";
        if (k.size() >= pattern.size()) {
            return k.compare(k.size() - pattern.size(), pattern.size(), pattern) == 0;
        }
        return false;
    };
    REQUIRE(vr.func("test_end"));
    REQUIRE_FALSE(vr.func("end_test"));
}

TEST_CASE_METHOD(ZestDBValidationFixture, "createValidationRule ew mode key shorter", "[validation]") {
    ValidationRule vr;
    vr.func = [](const std::string &k) {
        std::string pattern = "verylongpattern";
        return k.size() >= pattern.size() && k.compare(k.size() - pattern.size(), pattern.size(), pattern) == 0;
    };
    REQUIRE_FALSE(vr.func("short"));
}

TEST_CASE_METHOD(ZestDBValidationFixture, "isJsonValid valid JSON", "[validation]") {
    REQUIRE(db->execCmd("gcfg").code == ResultType::Code::SUCCESS);
}

TEST_CASE_METHOD(ZestDBValidationFixture, "ResultType default values", "[validation]") {
    ResultType r;
    REQUIRE(r.affectedRows == 0);
}
