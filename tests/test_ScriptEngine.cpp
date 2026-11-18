#include <catch2/catch_all.hpp>
#include <filesystem>
#include <format>
#include <fstream>
#include <iostream>
#include <sstream>
#include <future>
#include <string>
#include <thread>
#include <vector>

#include "ScriptEngine.hpp"
#include "ZestDB.hpp"
#include "lib/json.hpp"

using namespace Zest;
namespace fs = std::filesystem;

class ScriptFixture {
public:
    fs::path testDir;
    fs::path originalCwd;
    ZestDB* db = nullptr;

    ScriptFixture() {
        originalCwd = fs::current_path();
        testDir = fs::temp_directory_path() / ("zestdb_test_scripts_" + std::to_string(reinterpret_cast<uintptr_t>(this)));
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
        cfg << "DBPort: 29541\n";
        cfg << "WebPort: 29542\n";
        cfg << "useSSL: false\n";
        cfg << "NetworkValidation: .*\n";
        cfg << "ArchiveStoragePath: " << (testDir / "archive").string() << "\n";
        cfg << "ArchiveCreationDelay: 3600\n";
        cfg << "AutoArchiveSaving: false\n";
        cfg.close();

        fs::current_path(testDir);
        db = new ZestDB();
    }

    ~ScriptFixture() {
        if (db) {
            db->stop();
            delete db;
        }
        fs::current_path(originalCwd);
        fs::remove_all(testDir);
    }

    void restart() {
        db->stop();
        delete db;
        db = new ZestDB();
    }

    ResultType load(const std::string& name, const std::string& code, Role role = Role::Admin) {
        std::string quoted;
        for (char c : code) {
            if (c == '"' || c == '\\') {
                quoted += '\\';
            }
            quoted += c;
        }
        return db->execCmd(std::format("script load {} \"{}\"", name, quoted), role);
    }
};

TEST_CASE_METHOD(ScriptFixture, "script load registers a function that call runs", "[scripts]") {
    REQUIRE(load("hello", R"lua(
        zest.register_function("hello", function(name) return "hello " .. name end)
    )lua").code == ResultType::Code::Success);

    auto r = db->execCmd(R"(call hello "bob")");
    REQUIRE(r.code == ResultType::Code::Success);
    REQUIRE(r.response == "hello bob");
}

TEST_CASE_METHOD(ScriptFixture, "script load writes the file under DbPath/scripts", "[scripts]") {
    REQUIRE(load("lib", R"lua(zest.register_function("f", function() return "1" end))lua").code == ResultType::Code::Success);
    REQUIRE(fs::is_regular_file(testDir / "db" / "scripts" / "lib.lua"));
}

TEST_CASE_METHOD(ScriptFixture, "script load rejects a syntax error and changes nothing", "[scripts]") {
    REQUIRE(load("bad", "zest.register_function(\"f\", function() return end").code == ResultType::Code::ScriptError);
    REQUIRE_FALSE(fs::exists(testDir / "db" / "scripts" / "bad.lua"));
    REQUIRE(db->execCmd("call f").code == ResultType::Code::FunctionNotFound);
}

TEST_CASE_METHOD(ScriptFixture, "script load rejects invalid script names", "[scripts]") {
    REQUIRE(load("../evil", R"lua(zest.register_function("f", function() end))lua").code == ResultType::Code::ScriptError);
    REQUIRE(load("a/b", R"lua(zest.register_function("f", function() end))lua").code == ResultType::Code::ScriptError);
    REQUIRE_FALSE(fs::exists(testDir / "db" / "evil.lua"));
}

TEST_CASE_METHOD(ScriptFixture, "a script cannot read or write data while loading", "[scripts]") {
    REQUIRE(load("side", R"lua(zest.set("loaded", "yes"))lua").code == ResultType::Code::ScriptError);
    REQUIRE(load("peek", R"lua(local v = zest.get("k"))lua").code == ResultType::Code::ScriptError);
    REQUIRE(db->execCmd("g loaded").code == ResultType::Code::KeyNotFound);
}

TEST_CASE_METHOD(ScriptFixture, "a function name can only be registered by one script", "[scripts]") {
    REQUIRE(load("a", R"lua(zest.register_function("f", function() return "a" end))lua").code == ResultType::Code::Success);
    REQUIRE(load("b", R"lua(zest.register_function("f", function() return "b" end))lua").code == ResultType::Code::ScriptError);
    REQUIRE(db->execCmd("call f").response == "a");
}

TEST_CASE_METHOD(ScriptFixture, "reloading a script replaces everything it registered", "[scripts]") {
    REQUIRE(load("lib", R"lua(
        zest.register_function("old", function() return "old" end)
    )lua").code == ResultType::Code::Success);
    REQUIRE(load("lib", R"lua(
        zest.register_function("new", function() return "new" end)
    )lua").code == ResultType::Code::Success);

    REQUIRE(db->execCmd("call old").code == ResultType::Code::FunctionNotFound);
    REQUIRE(db->execCmd("call new").response == "new");
}

TEST_CASE_METHOD(ScriptFixture, "script del removes the script with its functions and triggers", "[scripts]") {
    REQUIRE(load("lib", R"lua(
        zest.register_function("f", function() return "1" end)
        zest.register_trigger{ name = "reject_all", phase = "before", event = "set",
            fn = function(key, value) error("no") end }
    )lua").code == ResultType::Code::Success);
    REQUIRE(db->execCmd(R"(s k "v")").code == ResultType::Code::TriggerRejected);

    REQUIRE(db->execCmd("script del lib").code == ResultType::Code::Success);
    REQUIRE(db->execCmd("call f").code == ResultType::Code::FunctionNotFound);
    REQUIRE(db->execCmd(R"(s k "v")").code == ResultType::Code::Success);
    REQUIRE_FALSE(fs::exists(testDir / "db" / "scripts" / "lib.lua"));
}

TEST_CASE_METHOD(ScriptFixture, "script del of an unknown script fails", "[scripts]") {
    REQUIRE(db->execCmd("script del ghost").code == ResultType::Code::ScriptNotFound);
}

TEST_CASE_METHOD(ScriptFixture, "script ls lists the scripts", "[scripts]") {
    REQUIRE(load("one", R"lua(zest.register_function("f1", function() end))lua").code == ResultType::Code::Success);
    REQUIRE(load("two", R"lua(zest.register_function("f2", function() end))lua").code == ResultType::Code::Success);

    auto r = db->execCmd("script ls");
    REQUIRE(r.code == ResultType::Code::Success);
    REQUIRE(r.response.find("one") != std::string::npos);
    REQUIRE(r.response.find("two") != std::string::npos);
}

TEST_CASE_METHOD(ScriptFixture, "scripts, functions and triggers survive a restart", "[scripts]") {
    REQUIRE(load("lib", R"lua(
        zest.register_function("f", function() return "still here" end)
        zest.register_trigger{ name = "upper", phase = "before", event = "set", mode = "sw", pattern = "up:",
            fn = function(key, value) return value:upper() end }
    )lua").code == ResultType::Code::Success);

    restart();

    REQUIRE(db->execCmd("call f").response == "still here");
    REQUIRE(db->execCmd(R"(s up:1 "abc")").code == ResultType::Code::Success);
    REQUIRE(db->execCmd("g up:1").response == "ABC");
}

TEST_CASE_METHOD(ScriptFixture, "an invalid file in DbPath/scripts does not stop the server", "[scripts]") {
    REQUIRE(load("good", R"lua(zest.register_function("f", function() return "ok" end))lua").code == ResultType::Code::Success);
    db->stop();
    delete db;
    std::ofstream(testDir / "db" / "scripts" / "broken.lua") << "this is not lua (";
    db = new ZestDB();

    REQUIRE(db->execCmd("call f").response == "ok");
}

TEST_CASE_METHOD(ScriptFixture, "script commands require the admin role and call the write role", "[scripts]") {
    const std::string code = R"lua(zest.register_function("f", function() return "1" end))lua";
    REQUIRE(load("lib", code, Role::Write).code == ResultType::Code::PermissionDenied);
    REQUIRE(db->execCmd("script ls", Role::Write).code == ResultType::Code::PermissionDenied);
    REQUIRE(db->execCmd("script del lib", Role::Write).code == ResultType::Code::PermissionDenied);

    REQUIRE(load("lib", code).code == ResultType::Code::Success);
    REQUIRE(db->execCmd("call f", Role::Read).code == ResultType::Code::PermissionDenied);
    REQUIRE(db->execCmd("call f", Role::Write).code == ResultType::Code::Success);
}

TEST_CASE_METHOD(ScriptFixture, "register_trigger needs mode and pattern together", "[scripts][triggers]") {
    REQUIRE(load("only_mode", R"lua(
        zest.register_trigger{ name = "t", phase = "before", event = "set", mode = "sw", fn = function(k, v) return v end }
    )lua").code == ResultType::Code::ScriptError);
    REQUIRE(load("only_pattern", R"lua(
        zest.register_trigger{ name = "t", phase = "before", event = "set", pattern = "a:", fn = function(k, v) return v end }
    )lua").code == ResultType::Code::ScriptError);
}

TEST_CASE_METHOD(ScriptFixture, "register_trigger rejects invalid definitions", "[scripts][triggers]") {
    SECTION("missing name") {
        REQUIRE(load("t", R"lua(zest.register_trigger{ phase = "before", event = "set", fn = function(k, v) return v end })lua").code == ResultType::Code::ScriptError);
    }
    SECTION("unknown phase") {
        REQUIRE(load("t", R"lua(zest.register_trigger{ name = "t", phase = "during", event = "set", fn = function(k, v) return v end })lua").code == ResultType::Code::ScriptError);
    }
    SECTION("unknown event") {
        REQUIRE(load("t", R"lua(zest.register_trigger{ name = "t", phase = "before", event = "get", fn = function(k, v) return v end })lua").code == ResultType::Code::ScriptError);
    }
    SECTION("unknown mode") {
        REQUIRE(load("t", R"lua(zest.register_trigger{ name = "t", phase = "before", event = "set", mode = "xx", pattern = "a", fn = function(k, v) return v end })lua").code == ResultType::Code::ScriptError);
    }
    SECTION("invalid regex") {
        REQUIRE(load("t", R"lua(zest.register_trigger{ name = "t", phase = "before", event = "set", mode = "re", pattern = "(", fn = function(k, v) return v end })lua").code == ResultType::Code::ScriptError);
    }
    SECTION("missing fn") {
        REQUIRE(load("t", R"lua(zest.register_trigger{ name = "t", phase = "before", event = "set" })lua").code == ResultType::Code::ScriptError);
    }
}

TEST_CASE_METHOD(ScriptFixture, "register_* cannot be called once the script is loaded", "[scripts]") {
    REQUIRE(load("lib", R"lua(
        zest.register_function("sneaky", function()
            zest.register_function("other", function() return "x" end)
        end)
    )lua").code == ResultType::Code::Success);

    REQUIRE(db->execCmd("call sneaky").code == ResultType::Code::ScriptError);
    REQUIRE(db->execCmd("call other").code == ResultType::Code::FunctionNotFound);
}

TEST_CASE_METHOD(ScriptFixture, "a before trigger rejects matching writes only", "[scripts][triggers]") {
    REQUIRE(load("json", R"lua(
        zest.register_trigger{ name = "user_json", phase = "before", event = "set", mode = "sw", pattern = "user:",
            fn = function(key, value)
                if not value:match("^%s*{") then error("JSON attendu") end
                return value
            end }
    )lua").code == ResultType::Code::Success);

    REQUIRE(db->execCmd(R"(s user:1 "plain")").code == ResultType::Code::TriggerRejected);
    REQUIRE(db->execCmd("g user:1").code == ResultType::Code::KeyNotFound);

    REQUIRE(db->execCmd(R"(s user:2 "{\"a\":1}")").code == ResultType::Code::Success);
    REQUIRE(db->execCmd(R"(s other "plain")").code == ResultType::Code::Success);
}

TEST_CASE_METHOD(ScriptFixture, "a before trigger can transform the value", "[scripts][triggers]") {
    REQUIRE(load("trim", R"lua(
        zest.register_trigger{ name = "trim", phase = "before", event = "set", mode = "sw", pattern = "name:",
            fn = function(key, value) return (value:gsub("^%s+", ""):gsub("%s+$", "")) end }
    )lua").code == ResultType::Code::Success);

    REQUIRE(db->execCmd(R"(s name:1 "  bob  ")").code == ResultType::Code::Success);
    REQUIRE(db->execCmd("g name:1").response == "bob");
}

TEST_CASE_METHOD(ScriptFixture, "a trigger without mode and pattern applies to every key", "[scripts][triggers]") {
    REQUIRE(load("nonempty", R"lua(
        zest.register_trigger{ name = "no_blank", phase = "before", event = "set",
            fn = function(key, value)
                if value:match("^%s*$") then error("blank") end
                return value
            end }
    )lua").code == ResultType::Code::Success);

    REQUIRE(db->execCmd(R"(s a "  ")").code == ResultType::Code::TriggerRejected);
    REQUIRE(db->execCmd(R"(s some:other:key " ")").code == ResultType::Code::TriggerRejected);
    REQUIRE(db->execCmd(R"(s a "x")").code == ResultType::Code::Success);
}

TEST_CASE_METHOD(ScriptFixture, "triggers apply to every write command of their event", "[scripts][triggers]") {
    REQUIRE(load("upper", R"lua(
        zest.register_trigger{ name = "upper", phase = "before", event = "set", mode = "sw", pattern = "up:",
            fn = function(key, value) return value:upper() end }
    )lua").code == ResultType::Code::Success);

    REQUIRE(db->execCmd(R"(setex up:ttl 60 "a")").code == ResultType::Code::Success);
    REQUIRE(db->execCmd("g up:ttl").response == "A");
    REQUIRE(db->execCmd(R"(setnx up:nx "b")").code == ResultType::Code::Success);
    REQUIRE(db->execCmd("g up:nx").response == "B");
}

TEST_CASE_METHOD(ScriptFixture, "an after trigger runs once the write is done", "[scripts][triggers]") {
    REQUIRE(load("audit", R"lua(
        zest.register_trigger{ name = "count_orders", phase = "after", event = "set", mode = "sw", pattern = "order:",
            fn = function(key, value) zest.incr("stats:orders") end }
        zest.register_trigger{ name = "last_deleted", phase = "after", event = "del", mode = "sw", pattern = "order:",
            fn = function(key) zest.set("stats:last_deleted", key) end }
    )lua").code == ResultType::Code::Success);

    db->execCmd(R"(s order:1 "a")");
    db->execCmd(R"(s order:2 "b")");
    db->execCmd(R"(s other "c")");
    REQUIRE(db->execCmd("g stats:orders").response == "2");

    REQUIRE(db->execCmd("d order:1").code == ResultType::Code::Success);
    REQUIRE(db->execCmd("g stats:last_deleted").response == "order:1");
}

TEST_CASE_METHOD(ScriptFixture, "an after trigger sees the value a before trigger stored", "[scripts][triggers]") {
    REQUIRE(load("chain", R"lua(
        zest.register_trigger{ name = "upper", phase = "before", event = "set", mode = "sw", pattern = "c:",
            fn = function(key, value) return value:upper() end }
        zest.register_trigger{ name = "copy", phase = "after", event = "set", mode = "sw", pattern = "c:",
            fn = function(key, value) zest.set("copy:" .. key, value) end }
    )lua").code == ResultType::Code::Success);

    db->execCmd(R"(s c:1 "abc")");
    REQUIRE(db->execCmd("g copy:c:1").response == "ABC");
}

TEST_CASE_METHOD(ScriptFixture, "writes made by a function fire the triggers", "[scripts][triggers]") {
    REQUIRE(load("lib", R"lua(
        zest.register_trigger{ name = "no_neg", phase = "before", event = "set", mode = "sw", pattern = "acc:",
            fn = function(key, value)
                if tonumber(value) < 0 then error("negative") end
                return value
            end }
        zest.register_function("put", function(key, value) zest.set(key, value) end)
    )lua").code == ResultType::Code::Success);

    REQUIRE(db->execCmd(R"(call put "acc:1" "5")").code == ResultType::Code::Success);
    REQUIRE(db->execCmd(R"(call put "acc:1" "-5")").code == ResultType::Code::ScriptError);
    REQUIRE(db->execCmd("g acc:1").response == "5");
}

TEST_CASE_METHOD(ScriptFixture, "triggers that call each other forever stop at the recursion limit", "[scripts][triggers]") {
    REQUIRE(load("loop", R"lua(
        zest.register_trigger{ name = "ping", phase = "after", event = "set", mode = "sw", pattern = "loop:",
            fn = function(key, value) zest.set(key, tostring(tonumber(value) + 1)) end }
    )lua").code == ResultType::Code::Success);

    REQUIRE(db->execCmd(R"(s loop:1 "0")").code == ResultType::Code::ScriptLimitExceeded);
    REQUIRE(db->execCmd(R"(s plain "still works")").code == ResultType::Code::Success);
}

TEST_CASE_METHOD(ScriptFixture, "setby and delby fire the triggers for every row", "[scripts][triggers]") {
    REQUIRE(load("lib", R"lua(
        zest.register_trigger{ name = "upper", phase = "before", event = "set", mode = "sw", pattern = "row:",
            fn = function(key, value) return value:upper() end }
        zest.register_trigger{ name = "count_del", phase = "after", event = "del", mode = "sw", pattern = "row:",
            fn = function(key) zest.incr("stats:deleted") end }
    )lua").code == ResultType::Code::Success);

    for (int i = 0; i < 5; i++) {
        db->execCmd(std::format("s row:{} \"x\"", i));
    }
    REQUIRE(db->execCmd(R"(sb sw row: "abc")").affectedRows == 5);
    REQUIRE(db->execCmd("g row:3").response == "ABC");

    REQUIRE(db->execCmd("db sw row:").affectedRows == 5);
    REQUIRE(db->execCmd("g stats:deleted").response == "5");
}

TEST_CASE_METHOD(ScriptFixture, "triggers are not run again when the WAL is replayed", "[scripts][triggers]") {
    REQUIRE(load("audit", R"lua(
        zest.register_trigger{ name = "count", phase = "after", event = "set", mode = "sw", pattern = "ev:",
            fn = function(key, value) zest.incr("stats:events") end }
    )lua").code == ResultType::Code::Success);

    db->execCmd(R"(s ev:1 "a")");
    db->execCmd(R"(s ev:2 "b")");
    REQUIRE(db->execCmd("g stats:events").response == "2");

    restart();

    REQUIRE(db->execCmd("g stats:events").response == "2");
    REQUIRE(db->execCmd("g ev:2").response == "b");
}

TEST_CASE_METHOD(ScriptFixture, "no trigger means writes behave as before", "[scripts][triggers]") {
    REQUIRE(load("lib", R"lua(zest.register_function("f", function() end))lua").code == ResultType::Code::Success);
    REQUIRE(db->execCmd(R"(s k "v")").code == ResultType::Code::Success);
    REQUIRE(db->execCmd("d k").code == ResultType::Code::Success);
}

TEST_CASE_METHOD(ScriptFixture, "call passes the arguments as strings", "[scripts][call]") {
    REQUIRE(load("lib", R"lua(
        zest.register_function("join", function(...) return table.concat({...}, ",") end)
        zest.register_function("count", function(...) return tostring(select("#", ...)) end)
    )lua").code == ResultType::Code::Success);

    REQUIRE(db->execCmd(R"(call join "a" "b c" "d\"e")").response == R"(a,b c,d"e)");
    REQUIRE(db->execCmd("call count").response == "0");
}

TEST_CASE_METHOD(ScriptFixture, "call of an unknown function fails", "[scripts][call]") {
    REQUIRE(db->execCmd("call nope").code == ResultType::Code::FunctionNotFound);
}

TEST_CASE_METHOD(ScriptFixture, "a procedure runs several operations in one call", "[scripts][call]") {
    REQUIRE(load("bank", R"lua(
        zest.register_function("transfer", function(from, to, amount)
            amount = tonumber(amount)
            if zest.incr(from, -amount) < 0 then
                zest.incr(from, amount)
                error("solde insuffisant")
            end
            zest.incr(to, amount)
        end)
    )lua").code == ResultType::Code::Success);
    db->execCmd(R"(s alice "100")");
    db->execCmd(R"(s bob "0")");

    REQUIRE(db->execCmd(R"(call transfer "alice" "bob" "30")").code == ResultType::Code::Success);
    REQUIRE(db->execCmd("g alice").response == "70");
    REQUIRE(db->execCmd("g bob").response == "30");

    REQUIRE(db->execCmd(R"(call transfer "alice" "bob" "500")").code == ResultType::Code::ScriptError);
    REQUIRE(db->execCmd("g alice").response == "70");
    REQUIRE(db->execCmd("g bob").response == "30");
}

TEST_CASE_METHOD(ScriptFixture, "the zest API maps to the database commands", "[scripts][call]") {
    REQUIRE(load("api", R"lua(
        zest.register_function("api", function()
            zest.set("a", "1")
            assert(zest.get("a") == "1")
            assert(zest.get("missing") == nil)
            assert(zest.exists("a") and not zest.exists("missing"))
            assert(zest.incr("n", 5) == 5)
            assert(zest.setnx("a", "2") == false)
            assert(zest.cas("a", "1", "3") == true)
            assert(zest.get("a") == "3")
            zest.set("t", "x", 60)
            assert(zest.ttl("t") > 0)
            zest.persist("t")
            assert(zest.ttl("t") == -1)
            for i = 1, 3 do zest.set("p" .. i, "v") end
            assert(zest.count("sw", "p") == 3)
            assert(#zest.keys("sw", "p", 2) == 2)
            zest.del("a")
            assert(zest.get("a") == nil)
            return "ok"
        end)
    )lua").code == ResultType::Code::Success);

    auto r = db->execCmd("call api");
    REQUIRE(r.code == ResultType::Code::Success);
    REQUIRE(r.response == "ok");
}

TEST_CASE_METHOD(ScriptFixture, "writes made by a procedure survive a restart", "[scripts][call]") {
    REQUIRE(load("lib", R"lua(
        zest.register_function("fill", function()
            for i = 1, 3 do zest.set("p:" .. i, "v" .. i) end
        end)
    )lua").code == ResultType::Code::Success);
    REQUIRE(db->execCmd("call fill").code == ResultType::Code::Success);

    restart();

    REQUIRE(db->execCmd("g p:1").response == "v1");
    REQUIRE(db->execCmd("g p:3").response == "v3");
}

TEST_CASE_METHOD(ScriptFixture, "a procedure that writes respects read-only mode", "[scripts][call]") {
    REQUIRE(load("lib", R"lua(zest.register_function("w", function() zest.set("k", "v") end))lua").code == ResultType::Code::Success);
    REQUIRE(db->execCmd("scfg readonly true").code == ResultType::Code::Success);

    REQUIRE(db->execCmd("call w").code != ResultType::Code::Success);
    REQUIRE(db->execCmd("g k").code == ResultType::Code::KeyNotFound);
}

TEST_CASE_METHOD(ScriptFixture, "dangerous libraries are not available", "[scripts][sandbox]") {
    REQUIRE(load("probe", R"lua(
        zest.register_function("has", function(name)
            local ok, v = pcall(function() return load("return " .. name)() end)
            return tostring(ok and v ~= nil)
        end)
    )lua").code == ResultType::Code::Success);

    for (const char* name : { "io", "os", "package", "debug", "require", "dofile", "loadfile", "collectgarbage" }) {
        INFO(name);
        REQUIRE(db->execCmd(std::format("call has \"{}\"", name)).response == "false");
    }
    for (const char* name : { "string", "table", "math", "pairs", "tostring" }) {
        INFO(name);
        REQUIRE(db->execCmd(std::format("call has \"{}\"", name)).response == "true");
    }
}

TEST_CASE_METHOD(ScriptFixture, "precompiled bytecode is refused", "[scripts][sandbox]") {
    REQUIRE(load("bc", R"lua(
        zest.register_function("bc", function()
            local f = load(string.dump(function() return 1 end))
            return tostring(f ~= nil)
        end)
    )lua").code == ResultType::Code::Success);

    auto r = db->execCmd("call bc");
    REQUIRE((r.code != ResultType::Code::Success || r.response == "false"));
}

TEST_CASE_METHOD(ScriptFixture, "an infinite loop stops at the instruction limit", "[scripts][sandbox]") {
    REQUIRE(load("spin", R"lua(zest.register_function("spin", function() while true do end end))lua").code == ResultType::Code::Success);

    REQUIRE(db->execCmd("call spin").code == ResultType::Code::ScriptLimitExceeded);
    REQUIRE(db->execCmd(R"(s k "v")").code == ResultType::Code::Success);
}

TEST_CASE_METHOD(ScriptFixture, "an infinite loop at load time is refused", "[scripts][sandbox]") {
    REQUIRE(load("spin", "while true do end").code == ResultType::Code::ScriptLimitExceeded);
}

TEST_CASE_METHOD(ScriptFixture, "a script that allocates without limit is stopped", "[scripts][sandbox]") {
    REQUIRE(load("hog", R"lua(
        zest.register_function("hog", function()
            local t = {}
            for i = 1, 1e9 do t[i] = string.rep("x", 1024) end
        end)
    )lua").code == ResultType::Code::Success);

    REQUIRE(db->execCmd("call hog").code == ResultType::Code::ScriptLimitExceeded);
}

TEST_CASE_METHOD(ScriptFixture, "functions run correctly from several threads at once", "[scripts][threads]") {
    REQUIRE(load("lib", R"lua(zest.register_function("bump", function(key) return tostring(zest.incr(key)) end))lua").code == ResultType::Code::Success);

    constexpr int threads = 8;
    constexpr int calls = 200;
    std::vector<std::future<int>> futures;
    for (int t = 0; t < threads; t++) {
        futures.push_back(std::async(std::launch::async, [this]() {
            int failures = 0;
            for (int i = 0; i < calls; i++) {
                failures += db->execCmd(R"(call bump "shared")").code != ResultType::Code::Success;
            }
            return failures;
        }));
    }
    for (auto& f : futures) {
        REQUIRE(f.get() == 0);
    }
    REQUIRE(db->execCmd("g shared").response == std::to_string(threads * calls));
}

TEST_CASE_METHOD(ScriptFixture, "a thread that already ran a script sees its new version", "[scripts][threads]") {
    REQUIRE(load("lib", R"lua(zest.register_function("v", function() return "1" end))lua").code == ResultType::Code::Success);

    std::promise<void> firstDone, reloaded;
    std::string before, after;
    std::thread worker([&]() {
        before = db->execCmd("call v").response;
        firstDone.set_value();
        reloaded.get_future().wait();
        after = db->execCmd("call v").response;
    });

    firstDone.get_future().wait();
    REQUIRE(load("lib", R"lua(zest.register_function("v", function() return "2" end))lua").code == ResultType::Code::Success);
    reloaded.set_value();
    worker.join();

    REQUIRE(before == "1");
    REQUIRE(after == "2");
}

TEST_CASE_METHOD(ScriptFixture, "ZestDB::loadScript and ZestDB::call", "[scripts][api]") {
    REQUIRE(db->loadScript("lib", R"lua(zest.register_function("add", function(a, b) return tostring(tonumber(a) + tonumber(b)) end))lua").code == ResultType::Code::Success);

    auto r = db->call("add", { "2", "40" });
    REQUIRE(r.code == ResultType::Code::Success);
    REQUIRE(r.response == "42");
    REQUIRE(db->call("missing", {}).code == ResultType::Code::FunctionNotFound);
}

TEST_CASE_METHOD(ScriptFixture, "ZestDB::loadScript keeps the code as is, without unquoting", "[scripts][api]") {
    REQUIRE(db->loadScript("lib", R"lua(zest.register_function("q", function() return "a\"b\\c" end))lua").code == ResultType::Code::Success);
    REQUIRE(db->call("q", {}).response == R"(a"b\c)");
}

TEST_CASE_METHOD(ScriptFixture, "ZestDB::loadScript errors", "[scripts][api]") {
    REQUIRE(db->loadScript("bad", "end end").code == ResultType::Code::ScriptError);
    REQUIRE(db->loadScript("", R"lua(zest.register_function("f", function() end))lua").code == ResultType::Code::ScriptError);
    REQUIRE(db->loadScript("empty", "").code == ResultType::Code::Success);
}

TEST_CASE_METHOD(ScriptFixture, "ZestDB::listScripts returns the scripts, functions and triggers as JSON", "[scripts][api]") {
    REQUIRE(db->loadScript("lib", R"lua(
        zest.register_function("f", function() end)
        zest.register_trigger{ name = "t", phase = "after", event = "del", fn = function(key) end }
    )lua").code == ResultType::Code::Success);

    auto r = db->listScripts();
    REQUIRE(r.code == ResultType::Code::Success);
    auto j = nlohmann::json::parse(r.response);
    REQUIRE(j["scripts"] == nlohmann::json::array({ "lib" }));
    REQUIRE(j["functions"] == nlohmann::json::array({ "f" }));
    REQUIRE(j["triggers"] == nlohmann::json::array({ "t" }));
}

TEST_CASE_METHOD(ScriptFixture, "ZestDB::listScripts on an empty database", "[scripts][api]") {
    auto r = db->listScripts();
    REQUIRE(r.code == ResultType::Code::Success);
    auto j = nlohmann::json::parse(r.response);
    REQUIRE(j["scripts"].empty());
    REQUIRE(j["functions"].empty());
    REQUIRE(j["triggers"].empty());
}

TEST_CASE_METHOD(ScriptFixture, "ZestDB::dropScript", "[scripts][api]") {
    REQUIRE(db->loadScript("lib", R"lua(zest.register_function("f", function() return "1" end))lua").code == ResultType::Code::Success);
    REQUIRE(db->dropScript("lib").code == ResultType::Code::Success);
    REQUIRE(db->dropScript("lib").code == ResultType::Code::ScriptNotFound);
    REQUIRE(db->call("f", {}).code == ResultType::Code::FunctionNotFound);
}

TEST_CASE_METHOD(ScriptFixture, "ZestDB::createValidationRule is usable from outside ZestDB", "[scripts][api]") {
    auto prefix = db->createValidationRule("sw", "user:");
    REQUIRE(prefix.result);
    REQUIRE(prefix.rule.func("user:1"));
    REQUIRE_FALSE(prefix.rule.func("order:1"));

    REQUIRE_FALSE(db->createValidationRule("xx", "a").result);
    REQUIRE_FALSE(db->createValidationRule("re", "(").result);
}

class EngineFixture : public ScriptFixture {
public:
    ScriptEngine engine{ *db, testDir / "engine_scripts" };
};

TEST_CASE_METHOD(EngineFixture, "ScriptEngine::hasTriggers follows the loaded scripts", "[scripts][engine]") {
    REQUIRE_FALSE(engine.hasTriggers());
    REQUIRE(engine.load("fns", R"lua(zest.register_function("f", function() end))lua").code == ResultType::Code::Success);
    REQUIRE_FALSE(engine.hasTriggers());

    REQUIRE(engine.load("trg", R"lua(zest.register_trigger{ name = "t", phase = "before", event = "set", fn = function(k, v) return v end })lua").code == ResultType::Code::Success);
    REQUIRE(engine.hasTriggers());

    REQUIRE(engine.drop("trg").code == ResultType::Code::Success);
    REQUIRE_FALSE(engine.hasTriggers());
}

TEST_CASE_METHOD(EngineFixture, "ScriptEngine::before transforms, rejects or ignores", "[scripts][engine]") {
    REQUIRE(engine.load("lib", R"lua(
        zest.register_trigger{ name = "up", phase = "before", event = "set", mode = "sw", pattern = "up:",
            fn = function(key, value)
                if value == "bad" then error("rejected") end
                return value:upper()
            end }
    )lua").code == ResultType::Code::Success);

    std::string value = "abc";
    REQUIRE(engine.before(TriggerEvent::Set, "up:1", value).code == ResultType::Code::Success);
    REQUIRE(value == "ABC");

    value = "bad";
    REQUIRE(engine.before(TriggerEvent::Set, "up:1", value).code == ResultType::Code::TriggerRejected);

    value = "abc";
    REQUIRE(engine.before(TriggerEvent::Set, "other", value).code == ResultType::Code::Success);
    REQUIRE(value == "abc");

    REQUIRE(engine.before(TriggerEvent::Del, "up:1", value).code == ResultType::Code::Success);
    REQUIRE(value == "abc");
}

TEST_CASE_METHOD(EngineFixture, "ScriptEngine::before must return a string", "[scripts][engine]") {
    REQUIRE(engine.load("lib", R"lua(
        zest.register_trigger{ name = "nothing", phase = "before", event = "set", fn = function(key, value) end }
    )lua").code == ResultType::Code::Success);

    std::string value = "abc";
    REQUIRE(engine.before(TriggerEvent::Set, "k", value).code == ResultType::Code::ScriptError);
}

TEST_CASE_METHOD(EngineFixture, "ScriptEngine::after runs the matching triggers through the database", "[scripts][engine]") {
    REQUIRE(engine.load("lib", R"lua(
        zest.register_trigger{ name = "log", phase = "after", event = "set", mode = "sw", pattern = "ev:",
            fn = function(key, value) zest.set("last", key .. "=" .. value) end }
    )lua").code == ResultType::Code::Success);

    REQUIRE(engine.after(TriggerEvent::Set, "ev:1", "x").code == ResultType::Code::Success);
    REQUIRE(db->get("last").response == "ev:1=x");

    REQUIRE(engine.after(TriggerEvent::Set, "other", "y").code == ResultType::Code::Success);
    REQUIRE(db->get("last").response == "ev:1=x");
}

TEST_CASE_METHOD(EngineFixture, "ScriptEngine::loadAll rebuilds the registry from the folder", "[scripts][engine]") {
    REQUIRE(engine.load("lib", R"lua(zest.register_function("f", function() return "1" end))lua").code == ResultType::Code::Success);

    ScriptEngine fresh{ *db, testDir / "engine_scripts" };
    REQUIRE(fresh.call("f", {}).code == ResultType::Code::FunctionNotFound);
    fresh.loadAll();
    REQUIRE(fresh.call("f", {}).response == "1");
}

TEST_CASE_METHOD(EngineFixture, "ScriptEngine::reload makes the threads use the reloaded code", "[scripts][engine]") {
    REQUIRE(engine.load("lib", R"lua(zest.register_function("v", function() return "1" end))lua").code == ResultType::Code::Success);
    REQUIRE(engine.call("v", {}).response == "1");

    std::ofstream(testDir / "engine_scripts" / "lib.lua", std::ios::trunc) << R"lua(zest.register_function("v", function() return "2" end))lua";
    REQUIRE(engine.call("v", {}).response == "1");
    engine.loadAll();
    engine.reload();
    REQUIRE(engine.call("v", {}).response == "2");
}

TEST_CASE_METHOD(EngineFixture, "ScriptEngine::list is sorted", "[scripts][engine]") {
    REQUIRE(engine.load("b", R"lua(zest.register_function("fb", function() end))lua").code == ResultType::Code::Success);
    REQUIRE(engine.load("a", R"lua(zest.register_function("fa", function() end))lua").code == ResultType::Code::Success);

    auto j = nlohmann::json::parse(engine.list().response);
    REQUIRE(j["scripts"] == nlohmann::json::array({ "a", "b" }));
    REQUIRE(j["functions"] == nlohmann::json::array({ "fa", "fb" }));
}

TEST_CASE_METHOD(ScriptFixture, "print and zest.log write to the server log", "[scripts][log]") {
    REQUIRE(load("lib", R"lua(
        zest.register_function("talk", function()
            print("hello", 42, true, nil)
            zest.log("careful", "warning")
            zest.log("plain")
        end)
        zest.register_function("bad_level", function() zest.log("x", "loud") end)
        print("loading")
    )lua").code == ResultType::Code::Success);

    std::ostringstream captured;
    std::streambuf* original = std::cout.rdbuf(captured.rdbuf());
    const ResultType talk = db->execCmd("call talk");
    std::cout.rdbuf(original);

    INFO(captured.str());
    REQUIRE(talk.code == ResultType::Code::Success);
    REQUIRE(captured.str().find(R"(Lua: hello\x0942\x09true\x09nil)") != std::string::npos);
    REQUIRE(captured.str().find("Lua: careful") != std::string::npos);
    REQUIRE(captured.str().find("Lua: plain") != std::string::npos);
    REQUIRE(db->execCmd("call bad_level").code == ResultType::Code::ScriptError);
}

TEST_CASE_METHOD(ScriptFixture, "script reload picks up a file edited on disk", "[scripts][reload]") {
    REQUIRE(load("lib", R"lua(zest.register_function("v", function() return "1" end))lua").code == ResultType::Code::Success);
    REQUIRE(load("other", R"lua(zest.register_function("o", function() return "other" end))lua").code == ResultType::Code::Success);

    std::ofstream(testDir / "db" / "scripts" / "lib.lua", std::ios::trunc) << R"lua(zest.register_function("v", function() return "2" end))lua";
    REQUIRE(db->execCmd("call v").response == "1");

    REQUIRE(db->execCmd("script reload lib").code == ResultType::Code::Success);
    REQUIRE(db->execCmd("call v").response == "2");
    REQUIRE(db->execCmd("call o").response == "other");
}

TEST_CASE_METHOD(ScriptFixture, "script reload loads a file added to the folder", "[scripts][reload]") {
    fs::create_directories(testDir / "db" / "scripts");
    std::ofstream(testDir / "db" / "scripts" / "dropped.lua") << R"lua(zest.register_function("d", function() return "here" end))lua";

    REQUIRE(db->execCmd("call d").code == ResultType::Code::FunctionNotFound);
    REQUIRE(db->reloadScript("dropped").code == ResultType::Code::Success);
    REQUIRE(db->execCmd("call d").response == "here");
}

TEST_CASE_METHOD(ScriptFixture, "script reload keeps the loaded version when the file is broken", "[scripts][reload]") {
    REQUIRE(load("lib", R"lua(zest.register_function("v", function() return "1" end))lua").code == ResultType::Code::Success);
    std::ofstream(testDir / "db" / "scripts" / "lib.lua", std::ios::trunc) << "this is not lua (";

    REQUIRE(db->execCmd("script reload lib").code == ResultType::Code::ScriptError);
    REQUIRE(db->execCmd("call v").response == "1");
}

TEST_CASE_METHOD(ScriptFixture, "script reload errors", "[scripts][reload]") {
    REQUIRE(db->execCmd("script reload ghost").code == ResultType::Code::ScriptNotFound);
    REQUIRE(db->execCmd("script reload ../config").code == ResultType::Code::ScriptError);
    REQUIRE(db->execCmd("script reload lib", Role::Write).code == ResultType::Code::PermissionDenied);
    REQUIRE(db->execCmd("script reload", Role::Write).code == ResultType::Code::PermissionDenied);
}

TEST_CASE_METHOD(ScriptFixture, "script reload without a name reloads the whole folder", "[scripts][reload]") {
    REQUIRE(load("edited", R"lua(zest.register_function("e", function() return "old" end))lua").code == ResultType::Code::Success);
    REQUIRE(load("removed", R"lua(zest.register_function("r", function() return "r" end))lua").code == ResultType::Code::Success);
    REQUIRE(load("kept", R"lua(zest.register_function("k", function() return "k" end))lua").code == ResultType::Code::Success);

    const fs::path dir = testDir / "db" / "scripts";
    std::ofstream(dir / "edited.lua", std::ios::trunc) << R"lua(zest.register_function("e", function() return "new" end))lua";
    std::ofstream(dir / "added.lua") << R"lua(zest.register_function("a", function() return "a" end))lua";
    std::ofstream(dir / "broken.lua") << "this is not lua (";
    fs::remove(dir / "removed.lua");

    auto r = db->execCmd("script reload");
    REQUIRE(r.code == ResultType::Code::Success);
    REQUIRE(r.affectedRows == 3);
    auto j = nlohmann::json::parse(r.response);
    REQUIRE(j["loaded"] == nlohmann::json::array({ "added", "edited", "kept" }));
    REQUIRE(j["failed"] == nlohmann::json::array({ "broken" }));

    REQUIRE(db->execCmd("call e").response == "new");
    REQUIRE(db->execCmd("call a").response == "a");
    REQUIRE(db->execCmd("call k").response == "k");
    REQUIRE(db->execCmd("call r").code == ResultType::Code::FunctionNotFound);
}

TEST_CASE_METHOD(ScriptFixture, "script reload of an empty folder unloads everything", "[scripts][reload]") {
    REQUIRE(db->reloadScripts().code == ResultType::Code::Success);
    REQUIRE(load("lib", R"lua(zest.register_function("f", function() return "1" end))lua").code == ResultType::Code::Success);
    fs::remove(testDir / "db" / "scripts" / "lib.lua");

    auto r = db->reloadScripts();
    REQUIRE(r.code == ResultType::Code::Success);
    REQUIRE(r.affectedRows == 0);
    REQUIRE(db->execCmd("call f").code == ResultType::Code::FunctionNotFound);
}

class ExamplesFixture : public ScriptFixture {
public:
    ExamplesFixture() {
        const fs::path dir = fs::path(__FILE__).parent_path().parent_path() / "scripts_examples";
        for (const auto& entry : fs::directory_iterator(dir)) {
            if (entry.path().extension() != ".lua") {
                continue;
            }
            std::ifstream in(entry.path(), std::ios::binary);
            std::stringstream code;
            code << in.rdbuf();
            const ResultType loaded = db->loadScript(entry.path().stem().string(), code.str());
            INFO(entry.path().filename().string());
            REQUIRE(loaded.code == ResultType::Code::Success);
            loadedCount++;
        }
    }

    std::string call(const std::string& command) {
        const ResultType r = db->execCmd(command);
        INFO(command);
        REQUIRE(r.code == ResultType::Code::Success);
        return r.response;
    }

    int loadedCount = 0;
};

TEST_CASE_METHOD(ExamplesFixture, "every example script loads together with the others", "[scripts][examples]") {
    REQUIRE(loadedCount == 9);
}

TEST_CASE_METHOD(ExamplesFixture, "example getting_started", "[scripts][examples]") {
    REQUIRE(call(R"(call hello "Ada")") == "Hello, Ada!");
    REQUIRE(db->execCmd("call hello").code == ResultType::Code::ScriptError);
    REQUIRE(call(R"(call visits "home")") == "1");
    REQUIRE(call(R"(call visits "home")") == "2");
}

TEST_CASE_METHOD(ExamplesFixture, "example validation", "[scripts][examples]") {
    call(R"(s email:1 "  Ada@Example.COM ")");
    REQUIRE(call("g email:1") == "ada@example.com");
    REQUIRE(db->execCmd(R"(s email:2 "not an email")").code == ResultType::Code::TriggerRejected);

    call(R"(s stock:sku1 "12.0")");
    REQUIRE(call("g stock:sku1") == "12");
    REQUIRE(db->execCmd(R"(s stock:sku1 "-1")").code == ResultType::Code::TriggerRejected);
    REQUIRE(db->execCmd(R"(s stock:sku1 "1.5")").code == ResultType::Code::TriggerRejected);

    call(R"(s note:1 "short")");
    REQUIRE(db->execCmd(std::format("s note:2 \"{}\"", std::string(281, 'x'))).code == ResultType::Code::TriggerRejected);

    call(R"(s config:app "v1")");
    REQUIRE(db->execCmd(R"(s config:app "v2")", Role::Write, true, "bob").code == ResultType::Code::TriggerRejected);
    REQUIRE(db->execCmd("d config:app").code == ResultType::Code::TriggerRejected);
    REQUIRE(call("g config:app") == "v1");
}

TEST_CASE_METHOD(ExamplesFixture, "example audit", "[scripts][examples]") {
    call(R"(s order:1 "2 pizzas")");
    call(R"(s order:2 "1 salad")");
    call("d order:1");
    REQUIRE(call("call order_stats") == "written=2 deleted=1 live=1");
    REQUIRE(call("g stats:orders:last") == "order:2");
    REQUIRE(std::stoi(call("ttl audit:order:1")) > 0);
    REQUIRE(call(R"(call order_history "5")") == "2: order:2 = 1 salad by admin\n1: order:1 = 2 pizzas by admin");

    REQUIRE(db->execCmd(R"(s order:3 "soup")", Role::Write, true, "bob").code == ResultType::Code::Success);
    REQUIRE(call("g audit:order:3") == "order:3 = soup by bob");
}

TEST_CASE_METHOD(ExamplesFixture, "example secondary_index", "[scripts][examples]") {
    call(R"(s user:1 "name=Ada;email=Ada@Example.com;city=London")");
    call(R"(s user:2 "name=Bob;email=bob@example.com;city=Paris")");
    REQUIRE(call(R"(call find_user_by_email "ada@example.com")") == "user:1 name=Ada;email=Ada@Example.com;city=London");
    REQUIRE(db->execCmd(R"(s user:3 "name=Eve;email=ADA@example.com")").code == ResultType::Code::TriggerRejected);
    REQUIRE(db->execCmd(R"(s user:3 "name=Eve")").code == ResultType::Code::TriggerRejected);
    REQUIRE(call(R"(call users_in_city "London")") == "user:1");

    call(R"(s user:1 "name=Ada;email=ada@new.io;city=London")");
    REQUIRE(call(R"(call find_user_by_email "ada@example.com")").empty());
    REQUIRE(call(R"(call find_user_by_email "ada@new.io")").starts_with("user:1 "));

    call("d user:1");
    REQUIRE(call(R"(call find_user_by_email "ada@new.io")").empty());
    REQUIRE(db->execCmd("g idx:user:email:ada@new.io").code == ResultType::Code::KeyNotFound);
}

TEST_CASE_METHOD(ExamplesFixture, "example bank", "[scripts][examples]") {
    REQUIRE(call(R"(call deposit "alice" "10000")") == "10000");
    REQUIRE(call(R"(call transfer "alice" "bob" "2500")") == "1");
    REQUIRE(call(R"(call balance "alice")") == "7500");
    REQUIRE(call(R"(call balance "bob")") == "2500");
    REQUIRE(call("g ledger:1") == "alice>bob:2500");

    REQUIRE(db->execCmd(R"(call transfer "alice" "bob" "999999")").code == ResultType::Code::ScriptError);
    REQUIRE(db->execCmd(R"(call withdraw "bob" "3000")").code == ResultType::Code::ScriptError);
    REQUIRE(db->execCmd(R"(call deposit "bob" "-5")").code == ResultType::Code::ScriptError);
    REQUIRE(call(R"(call balance "alice")") == "7500");
    REQUIRE(call(R"(call balance "bob")") == "2500");
    REQUIRE(call(R"(call withdraw "bob" "500")") == "2000");

    REQUIRE(db->execCmd(R"(s balance:alice "-5")").code == ResultType::Code::TriggerRejected);
}

TEST_CASE_METHOD(ExamplesFixture, "example rate_limit", "[scripts][examples]") {
    REQUIRE(call(R"(call allow "key-1" "2" "60")") == "allowed");
    REQUIRE(call(R"(call allow "key-1" "2" "60")") == "allowed");
    REQUIRE(call(R"(call allow "key-1" "2" "60")").starts_with("blocked:"));
    REQUIRE(call(R"(call allow "key-2" "2" "60")") == "allowed");
    REQUIRE(std::stoi(call("ttl ratelimit:key-1")) > 0);

    call(R"(call reset_limit "key-1")");
    REQUIRE(call(R"(call allow "key-1" "2" "60")") == "allowed");
    REQUIRE(db->execCmd(R"(call allow "key-1" "x" "60")").code == ResultType::Code::ScriptError);
}

TEST_CASE_METHOD(ExamplesFixture, "example sessions", "[scripts][examples]") {
    REQUIRE(call(R"(call session_open "tok-1" "alice" "1800")") == "session:tok-1");
    REQUIRE(db->execCmd(R"(call session_open "tok-1" "mallory" "1800")").code == ResultType::Code::ScriptError);
    REQUIRE(call(R"(call session_user "tok-1")") == "alice");
    REQUIRE(call(R"(call session_touch "tok-1" "60")") == "extended");
    REQUIRE(std::stoi(call("ttl session:tok-1")) <= 60);
    REQUIRE(call("call sessions_active") == "1");
    REQUIRE(call(R"(call session_close "tok-1")") == "closed");
    REQUIRE(call(R"(call session_close "tok-1")") == "unknown");
    REQUIRE(call(R"(call session_touch "tok-1" "60")") == "expired");
    REQUIRE(call("call sessions_active") == "0");
}

TEST_CASE_METHOD(ExamplesFixture, "example locks", "[scripts][examples]") {
    REQUIRE(call(R"(call lock_acquire "report" "worker-1" "300")") == "acquired");
    REQUIRE(call(R"(call lock_acquire "report" "worker-2" "300")") == "busy:worker-1");
    REQUIRE(std::stoi(call("ttl lock:report")) > 0);
    REQUIRE(call(R"(call lock_release "report" "worker-2")") == "not owner");
    REQUIRE(call(R"(call lock_owner "report")") == "worker-1");
    REQUIRE(call(R"(call lock_release "report" "worker-1")") == "released");
    REQUIRE(call(R"(call lock_release "report" "worker-1")") == "free");
    REQUIRE(call(R"(call lock_acquire "report" "worker-2" "300")") == "acquired");
}

TEST_CASE_METHOD(ExamplesFixture, "example maintenance", "[scripts][examples]") {
    for (int i = 0; i < 5; i++) {
        call(std::format("s tmp:{} \"x\"", i));
    }
    call(R"(s cache:a "1")");
    call(R"(setex template:invoice 3600 "Dear customer")");

    REQUIRE(call(R"(call inventory "tmp:" "cache:")") == "tmp:=5 cache:=1");
    REQUIRE(call(R"(call purge "sw" "tmp:" "3")") == "3");
    REQUIRE(call(R"(call purge "sw" "tmp:" "3")") == "2");
    REQUIRE(call(R"(call purge "sw" "tmp:" "3")") == "0");

    REQUIRE(call(R"(call expire_matching "sw" "cache:" "600")") == "1");
    REQUIRE(std::stoi(call("ttl cache:a")) > 0);
    REQUIRE(call(R"(call persist_matching "sw" "cache:")") == "1");
    REQUIRE(call("ttl cache:a") == "-1");

    REQUIRE(call(R"(call copy_key "template:invoice" "invoice:42")") == "copied");
    REQUIRE(call("g invoice:42") == "Dear customer");
    REQUIRE(std::stoi(call("ttl invoice:42")) > 0);
    REQUIRE(call(R"(call rename_key "invoice:42" "invoice:43")") == "renamed");
    REQUIRE(db->execCmd("g invoice:42").code == ResultType::Code::KeyNotFound);
    REQUIRE(db->execCmd(R"(call rename_key "template:invoice" "invoice:43")").code == ResultType::Code::ScriptError);
}

static bool contains(const std::string& text, const std::string& part) {
    return text.find(part) != std::string::npos;
}

TEST_CASE_METHOD(ScriptFixture, "a failing function returns its error message", "[scripts][errors]") {
    REQUIRE(load("lib", R"lua(
        zest.register_function("boom", function() error("kaboom") end)
        zest.register_function("ok", function() return "fine" end)
    )lua").code == ResultType::Code::Success);

    auto r = db->execCmd("call boom");
    REQUIRE(r.code == ResultType::Code::ScriptError);
    INFO(r.response);
    REQUIRE(contains(r.response, "lib.lua:2:"));
    REQUIRE(contains(r.response, "kaboom"));
    REQUIRE(contains(ZestDB::responseToJson(r), "kaboom"));

    REQUIRE(db->execCmd("call ok").response == "fine");
}

TEST_CASE_METHOD(ScriptFixture, "a rejecting trigger returns its error message", "[scripts][errors]") {
    REQUIRE(load("json", R"lua(
        zest.register_trigger{ name = "user_json", phase = "before", event = "set", mode = "sw", pattern = "user:",
            fn = function(key, value) error("JSON expected for " .. key) end }
        zest.register_trigger{ name = "no_string", phase = "before", event = "set", mode = "sw", pattern = "num:",
            fn = function(key, value) return 42 end }
    )lua").code == ResultType::Code::Success);

    auto rejected = db->execCmd(R"(s user:1 "plain")");
    REQUIRE(rejected.code == ResultType::Code::TriggerRejected);
    INFO(rejected.response);
    REQUIRE(contains(rejected.response, "JSON expected for user:1"));

    auto notString = db->execCmd(R"(s num:1 "1")");
    REQUIRE(notString.code == ResultType::Code::ScriptError);
    REQUIRE(contains(notString.response, "no_string must return the value to write"));
}

TEST_CASE_METHOD(ScriptFixture, "a failure deep in nested calls keeps the original message", "[scripts][errors]") {
    REQUIRE(load("lib", R"lua(
        zest.register_trigger{ name = "no_neg", phase = "before", event = "set", mode = "sw", pattern = "acc:",
            fn = function(key, value)
                if tonumber(value) < 0 then error("negative balance") end
                return value
            end }
        zest.register_function("put", function(key, value) zest.set(key, value) end)
        zest.register_function("write", function() zest.set("k", "v") end)
    )lua").code == ResultType::Code::Success);

    auto nested = db->execCmd(R"(call put "acc:1" "-5")");
    REQUIRE(nested.code == ResultType::Code::ScriptError);
    INFO(nested.response);
    REQUIRE(contains(nested.response, "code 603"));
    REQUIRE(contains(nested.response, "negative balance"));

    REQUIRE(db->execCmd("scfg readonly true").code == ResultType::Code::Success);
    auto readOnly = db->execCmd("call write");
    REQUIRE(readOnly.code == ResultType::Code::ScriptError);
    REQUIRE(contains(readOnly.response, "code 303"));
}

TEST_CASE_METHOD(ScriptFixture, "limits return a message saying which limit was reached", "[scripts][errors]") {
    REQUIRE(load("lib", R"lua(
        zest.register_function("spin", function() while true do end end)
        zest.register_function("hog", function()
            local t = {}
            for i = 1, 1e9 do t[i] = string.rep("x", 1024) end
        end)
        zest.register_function("alloc", function()
            local t = {}
            for i = 1, 10240 do t[i] = string.rep("y", 1024) end
            return #t
        end)
        zest.register_trigger{ name = "ping", phase = "after", event = "set", mode = "sw", pattern = "loop:",
            fn = function(key, value) zest.set(key, tostring(tonumber(value) + 1)) end }
    )lua").code == ResultType::Code::Success);

    auto spin = db->execCmd("call spin");
    REQUIRE(spin.code == ResultType::Code::ScriptLimitExceeded);
    REQUIRE(contains(spin.response, "too many instructions"));

    auto hog = db->execCmd("call hog");
    REQUIRE(hog.code == ResultType::Code::ScriptLimitExceeded);
    REQUIRE(contains(hog.response, "not enough memory"));
    REQUIRE(db->execCmd("call alloc").response == "10240");

    auto loop = db->execCmd(R"(s loop:1 "0")");
    INFO(loop.response);
    REQUIRE(loop.code == ResultType::Code::ScriptLimitExceeded);
    REQUIRE(contains(loop.response, "nested more than 16 levels"));
}

TEST_CASE_METHOD(ScriptFixture, "load errors return their message", "[scripts][errors]") {
    auto syntax = load("bad", "zest.register_function(\"f\", function() return end");
    REQUIRE(syntax.code == ResultType::Code::ScriptError);
    REQUIRE(contains(syntax.response, "bad.lua:1:"));

    auto loading = load("side", R"lua(zest.set("k", "v"))lua");
    REQUIRE(loading.code == ResultType::Code::ScriptError);
    REQUIRE(contains(loading.response, "cannot be called while the script loads"));

    auto definition = load("trg", R"lua(zest.register_trigger{ name = "t", phase = "before", event = "set", mode = "sw", fn = function(k, v) return v end })lua");
    REQUIRE(definition.code == ResultType::Code::ScriptError);
    REQUIRE(contains(definition.response, "mode and pattern go together"));

    REQUIRE(load("a", R"lua(zest.register_function("f", function() end))lua").code == ResultType::Code::Success);
    auto duplicate = load("b", R"lua(zest.register_function("f", function() end))lua");
    REQUIRE(duplicate.code == ResultType::Code::ScriptError);
    REQUIRE(contains(duplicate.response, "already uses"));

    REQUIRE(contains(load("../x", "").response, "invalid script name"));
    REQUIRE(contains(load("spin", "while true do end").response, "too many instructions"));
}

TEST_CASE_METHOD(ScriptFixture, "an error inside an after trigger is returned with the write already done", "[scripts][errors]") {
    REQUIRE(load("lib", R"lua(
        zest.register_trigger{ name = "audit", phase = "after", event = "set", mode = "sw", pattern = "o:",
            fn = function(key, value) error("audit store unavailable") end }
    )lua").code == ResultType::Code::Success);

    auto r = db->execCmd(R"(s o:1 "x")");
    REQUIRE(r.code == ResultType::Code::ScriptError);
    REQUIRE(contains(r.response, "audit store unavailable"));
    REQUIRE(db->execCmd("g o:1").response == "x");
}

TEST_CASE_METHOD(ScriptFixture, "zest.caller gives the user and role that sent the command", "[scripts][caller]") {
    REQUIRE(load("lib", R"lua(
        zest.register_function("who", function()
            local caller = zest.caller()
            if caller == nil then return "none" end
            return (caller.user or "-") .. ":" .. caller.role
        end)
    )lua").code == ResultType::Code::Success);

    REQUIRE(db->execCmd("call who", Role::Write, true, "alice").response == "alice:write");
    REQUIRE(db->execCmd("call who", Role::Admin, true, "root").response == "root:admin");
    REQUIRE(db->execCmd("call who").response == "-:admin");
    REQUIRE(db->call("who", {}).response == "none");
}

TEST_CASE_METHOD(ScriptFixture, "zest.caller is the client in triggers too, and is reset after the command", "[scripts][caller]") {
    REQUIRE(load("lib", R"lua(
        zest.register_trigger{ name = "stamp", phase = "after", event = "set", mode = "sw", pattern = "doc:",
            fn = function(key, value)
                local caller = zest.caller()
                zest.set("by:" .. key, caller and (caller.user or caller.role) or "server")
            end }
        zest.register_trigger{ name = "admin_only", phase = "before", event = "set", mode = "sw", pattern = "admin:",
            fn = function(key, value)
                local caller = zest.caller()
                if caller ~= nil and caller.role ~= "admin" then error("admins only") end
                return value
            end }
    )lua").code == ResultType::Code::Success);

    REQUIRE(db->execCmd(R"(s doc:1 "x")", Role::Write, true, "bob").code == ResultType::Code::Success);
    REQUIRE(db->get("by:doc:1").response == "bob");

    REQUIRE(db->set("doc:2", "y").code == ResultType::Code::Success);
    REQUIRE(db->get("by:doc:2").response == "server");

    auto denied = db->execCmd(R"(s admin:k "v")", Role::Write, true, "bob");
    REQUIRE(denied.code == ResultType::Code::TriggerRejected);
    REQUIRE(denied.response.find("admins only") != std::string::npos);
    REQUIRE(db->execCmd(R"(s admin:k "v")", Role::Admin, true, "root").code == ResultType::Code::Success);
}

TEST_CASE_METHOD(ScriptFixture, "zest.caller is not available while a script loads", "[scripts][caller]") {
    auto r = load("lib", "local c = zest.caller()");
    REQUIRE(r.code == ResultType::Code::ScriptError);
    REQUIRE(r.response.find("cannot be called while the script loads") != std::string::npos);
}

TEST_CASE_METHOD(ScriptFixture, "zest.caller is per thread", "[scripts][caller]") {
    REQUIRE(load("lib", R"lua(zest.register_function("who", function() local c = zest.caller() return c and c.user or "none" end))lua").code == ResultType::Code::Success);

    std::vector<std::future<bool>> futures;
    for (int t = 0; t < 4; t++) {
        futures.push_back(std::async(std::launch::async, [this, t]() {
            const std::string user = "user" + std::to_string(t);
            for (int i = 0; i < 100; i++) {
                if (db->execCmd("call who", Role::Write, true, user).response != user) {
                    return false;
                }
            }
            return true;
        }));
    }
    for (auto& f : futures) {
        REQUIRE(f.get());
    }
}
