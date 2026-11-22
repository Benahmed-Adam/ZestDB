#include <asio.hpp>
#include <atomic>
#include <catch2/catch_all.hpp>
#include <filesystem>
#include <format>
#include <fstream>
#include <future>
#include <thread>
#include <vector>

#include "ZestDB.hpp"

using namespace Zest;
namespace fs = std::filesystem;

static constexpr unsigned short TEST_DB_PORT = 29521;

class SessionFixture {
public:
    fs::path testDir;
    fs::path originalCwd;
    std::unique_ptr<ZestDB> db;
    std::vector<std::thread> ioThreads;

    static constexpr int MAX_CONNECTIONS = 16;

    void writeConfig(bool withAlice) {
        std::ofstream cfg(testDir / "config.yaml");
        cfg << "DbPath: " << (testDir / "db").string() << "\n";
        cfg << "MaxKeySize: 255\n";
        cfg << "DBPort: " << TEST_DB_PORT << "\n";
        cfg << "WebPort: 29522\n";
        cfg << "ArchiveStoragePath: " << (testDir / "archive").string() << "\n";
        cfg << "AutoArchiveSaving: false\n";
        cfg << "MaxConnections: " << MAX_CONNECTIONS << "\n";
        cfg << "AuthTimeout: 1\n";
        cfg << "IdleTimeout: 2\n";
        cfg << "users:\n";
        if (withAlice) {
            cfg << "  - user: alice\n";
            cfg << "    tokenHash: " << sha256(sha256("alicesecret")) << "\n";
        }
        cfg << "  - user: bob\n";
        cfg << "    password: bob\n";
        cfg << "  - user: reader\n";
        cfg << "    role: read\n";
        cfg << "    password: r\n";
        cfg << "  - user: root\n";
        cfg << "    role: admin\n";
        cfg << "    password: r\n";
    }

    SessionFixture() {
        originalCwd = fs::current_path();
        testDir = fs::temp_directory_path() / ("zestdb_test_session_" + std::to_string(reinterpret_cast<uintptr_t>(this)));
        fs::create_directories(testDir);
        writeConfig(true);

        fs::current_path(testDir);
        db = std::make_unique<ZestDB>();
        for (int i = 0; i < 4; i++) {
            ioThreads.emplace_back([this] { db->ioContext().run(); });
        }
    }

    ~SessionFixture() {
        db->stop();
        for (auto& t : ioThreads) {
            t.join();
        }
        db.reset();
        fs::current_path(originalCwd);
        fs::remove_all(testDir);
    }
};

static std::string be32(uint32_t v) {
    v = htonl(v);
    return std::string(reinterpret_cast<const char*>(&v), 4);
}

class Client {
public:
    asio::io_context ctx;
    asio::ip::tcp::socket sock{ ctx };

    Client() {
        sock.connect({ asio::ip::make_address("127.0.0.1"), TEST_DB_PORT });
    }

    std::string sendRaw(const std::string& bytes) {
        asio::error_code ec;
        asio::write(sock, asio::buffer(bytes), ec);
        if (ec) {
            return "";
        }
        asio::streambuf buf;
        asio::read_until(sock, buf, '\n', ec);
        return std::string(asio::buffers_begin(buf.data()), asio::buffers_end(buf.data()));
    }

    std::string send(const std::string& cmd) {
        std::string payload = be32(static_cast<uint32_t>(cmd.size())) + cmd;
        return sendRaw(be32(static_cast<uint32_t>(payload.size())) + payload);
    }
};

static std::string codeOf(ResultType::Code code) {
    return std::format("\"code\":{},", static_cast<int>(code));
}

TEST_CASE_METHOD(SessionFixture, "Session authenticates with a tokenHash user", "[session]") {
    Client c;
    REQUIRE(c.send("Authorization: alice." + sha256("alicesecret")).find(codeOf(ResultType::Code::Success)) != std::string::npos);
    REQUIRE(c.send(R"(s key1 "value1")").find(codeOf(ResultType::Code::Success)) != std::string::npos);
}

TEST_CASE_METHOD(SessionFixture, "Session still accepts a deprecated plaintext password user", "[session]") {
    Client c;
    REQUIRE(c.send("Authorization: bob." + sha256("bobbob")).find(codeOf(ResultType::Code::Success)) != std::string::npos);
}

TEST_CASE_METHOD(SessionFixture, "Session refuses a wrong token or the stored hash itself", "[session]") {
    Client wrong;
    REQUIRE(wrong.send("Authorization: alice.deadbeef").find(codeOf(ResultType::Code::AuthFailed)) != std::string::npos);
    Client storedHash;
    REQUIRE(storedHash.send("Authorization: alice." + sha256(sha256("alicesecret"))).find(codeOf(ResultType::Code::AuthFailed)) != std::string::npos);
}

TEST_CASE_METHOD(SessionFixture, "Session refuses an oversized payload before authentication", "[session]") {
    Client c;
    REQUIRE(c.sendRaw(be32(0xFFFFFFFF)).find(codeOf(ResultType::Code::PayloadTooLarge)) != std::string::npos);
}

TEST_CASE_METHOD(SessionFixture, "Session closes the connection on an Authorization without a token", "[session]") {
    Client c;
    REQUIRE(c.send("Authorization: bob").find(codeOf(ResultType::Code::AuthFailed)) != std::string::npos);
    REQUIRE(c.send("Authorization: bob." + sha256("bobbob")).empty());
}

static std::string login(Client& c, const std::string& user, const std::string& password) {
    return c.send("Authorization: " + user + "." + sha256(user + password));
}

TEST_CASE_METHOD(SessionFixture, "Session survives values that are not valid UTF-8", "[session]") {
    Client c;
    REQUIRE(login(c, "bob", "bob").find(codeOf(ResultType::Code::Success)) != std::string::npos);
    REQUIRE(c.send("s k \"\xff\xfe\"").find(codeOf(ResultType::Code::Success)) != std::string::npos);
    REQUIRE(c.send("g k").find(codeOf(ResultType::Code::Success)) != std::string::npos);
    REQUIRE(c.send("gb sw k").find(codeOf(ResultType::Code::Success)) != std::string::npos);
    REQUIRE(c.send(R"(s k2 "v")").find(codeOf(ResultType::Code::Success)) != std::string::npos);
}

TEST_CASE_METHOD(SessionFixture, "Session enforces the user's role", "[session]") {
    Client reader;
    REQUIRE(login(reader, "reader", "r").find(codeOf(ResultType::Code::Success)) != std::string::npos);
    REQUIRE(reader.send(R"(s k "v")").find(codeOf(ResultType::Code::PermissionDenied)) != std::string::npos);
    REQUIRE(reader.send("g k").find(codeOf(ResultType::Code::KeyNotFound)) != std::string::npos);

    Client writer;
    REQUIRE(login(writer, "bob", "bob").find(codeOf(ResultType::Code::Success)) != std::string::npos);
    REQUIRE(writer.send(R"(s k "v")").find(codeOf(ResultType::Code::Success)) != std::string::npos);
    for (const char* adminCmd : { "scfg readonly true", "gcfg", "r", "f", "save" }) {
        REQUIRE(writer.send(adminCmd).find(codeOf(ResultType::Code::PermissionDenied)) != std::string::npos);
    }

    Client admin;
    REQUIRE(login(admin, "root", "r").find(codeOf(ResultType::Code::Success)) != std::string::npos);
    REQUIRE(admin.send("gcfg").find(codeOf(ResultType::Code::Success)) != std::string::npos);
}

TEST_CASE_METHOD(SessionFixture, "Session is revoked when a reload removes its user", "[session]") {
    Client c;
    REQUIRE(c.send("Authorization: alice." + sha256("alicesecret")).find(codeOf(ResultType::Code::Success)) != std::string::npos);
    REQUIRE(c.send(R"(s k "v")").find(codeOf(ResultType::Code::Success)) != std::string::npos);
    writeConfig(false);
    REQUIRE(db->execCmd("r").code == ResultType::Code::Success);
    REQUIRE(c.send("g k").find(codeOf(ResultType::Code::SessionRevoked)) != std::string::npos);
}

TEST_CASE_METHOD(SessionFixture, "Session serves several clients in parallel", "[session]") {
    std::vector<std::thread> clients;
    std::atomic<int> failures = 0;
    for (int t = 0; t < 8; t++) {
        clients.emplace_back([t, &failures] {
            Client c;
            if (login(c, "bob", "bob").find(codeOf(ResultType::Code::Success)) == std::string::npos) {
                failures++;
                return;
            }
            for (int i = 0; i < 100; i++) {
                std::string key = std::format("c{}_{}", t, i);
                c.send("s " + key + " \"" + key + "\"");
                if (c.send("g " + key).find("\"response\":\"" + key + "\"") == std::string::npos) {
                    failures++;
                }
            }
        });
    }
    for (auto& c : clients) {
        c.join();
    }
    REQUIRE(failures == 0);
}

TEST_CASE_METHOD(SessionFixture, "Session refuses connections above MaxConnections", "[session]") {
    std::vector<std::unique_ptr<Client>> open;
    for (int i = 0; i < MAX_CONNECTIONS; i++) {
        open.push_back(std::make_unique<Client>());
        REQUIRE(login(*open.back(), "bob", "bob").find(codeOf(ResultType::Code::Success)) != std::string::npos);
    }
    Client extra;
    REQUIRE(login(extra, "bob", "bob").empty());
}

TEST_CASE_METHOD(SessionFixture, "Session closes a client that does not authenticate in time", "[session]") {
    Client c;
    std::this_thread::sleep_for(std::chrono::milliseconds(1500));
    REQUIRE(login(c, "bob", "bob").empty());
}

TEST_CASE_METHOD(SessionFixture, "Session closes an idle authenticated client", "[session]") {
    Client c;
    REQUIRE(login(c, "bob", "bob").find(codeOf(ResultType::Code::Success)) != std::string::npos);
    std::this_thread::sleep_for(std::chrono::milliseconds(2500));
    REQUIRE(c.send("g k").empty());
}

TEST_CASE_METHOD(SessionFixture, "Session locks a user out after repeated failed authentications, not the whole address", "[session]") {
    for (int i = 0; i < 5; i++) {
        Client c;
        REQUIRE(login(c, "bob", "wrong").find(codeOf(ResultType::Code::AuthFailed)) != std::string::npos);
    }
    Client lockedOut;
    REQUIRE(login(lockedOut, "bob", "bob").find(codeOf(ResultType::Code::AuthFailed)) != std::string::npos);

    Client other;
    REQUIRE(login(other, "reader", "r").find(codeOf(ResultType::Code::Success)) != std::string::npos);
}

TEST_CASE_METHOD(SessionFixture, "Session passes its user and role to the scripts", "[session][scripts]") {
    REQUIRE(db->loadScript("who", "zest.register_function('who', function() local c = zest.caller() return c.user .. ':' .. c.role end)").code == ResultType::Code::Success);

    Client writer;
    REQUIRE(login(writer, "bob", "bob").find(codeOf(ResultType::Code::Success)) != std::string::npos);
    REQUIRE(writer.send("call who").find("\"response\":\"bob:write\"") != std::string::npos);

    Client admin;
    REQUIRE(login(admin, "root", "r").find(codeOf(ResultType::Code::Success)) != std::string::npos);
    REQUIRE(admin.send("call who").find("\"response\":\"root:admin\"") != std::string::npos);
}

static size_t countOf(const std::string& text, const std::string& part) {
    size_t n = 0;
    for (size_t pos = text.find(part); pos != std::string::npos; pos = text.find(part, pos + part.size())) {
        n++;
    }
    return n;
}

TEST_CASE_METHOD(SessionFixture, "Stopping the server answers the request in progress before closing", "[session][shutdown]") {
    REQUIRE(db->loadScript("slow", "zest.register_function('slow', function(key) for i = 1, 4000000 do end zest.incr(key) return 'done' end)").code == ResultType::Code::Success);

    Client c;
    REQUIRE(login(c, "bob", "bob").find(codeOf(ResultType::Code::Success)) != std::string::npos);

    constexpr int calls = 40;
    std::string payload;
    for (int i = 0; i < calls; i++) {
        const std::string cmd = R"(call slow "counter")";
        payload += be32(static_cast<uint32_t>(cmd.size())) + cmd;
    }
    // The replies of a batch are separated by "\r\n\r\n": read up to the final "}\n", not the first "\n"
    auto reply = std::async(std::launch::async, [&c, &payload] {
        asio::error_code ec;
        asio::write(c.sock, asio::buffer(be32(static_cast<uint32_t>(payload.size())) + payload), ec);
        asio::streambuf buf;
        asio::read_until(c.sock, buf, "}\n", ec);
        return std::string(asio::buffers_begin(buf.data()), asio::buffers_end(buf.data()));
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    db->stop();

    const std::string answers = reply.get();
    INFO(answers.substr(0, 300));
    REQUIRE(countOf(answers, codeOf(ResultType::Code::Success)) == calls);
    REQUIRE(c.send("g counter").empty());
}

TEST_CASE_METHOD(SessionFixture, "Stopping the server closes idle sessions without waiting", "[session][shutdown]") {
    Client idle;
    REQUIRE(login(idle, "bob", "bob").find(codeOf(ResultType::Code::Success)) != std::string::npos);
    Client unauthenticated;

    const auto start = std::chrono::steady_clock::now();
    db->stop();
    REQUIRE(std::chrono::steady_clock::now() - start < std::chrono::seconds(3));

    REQUIRE(idle.send("g k").empty());
    REQUIRE(unauthenticated.send("Authorization: bob." + sha256("bobbob")).empty());
}
