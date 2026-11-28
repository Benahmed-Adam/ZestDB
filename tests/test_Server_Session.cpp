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
#include "lib/json.hpp"

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
        cfg << "  - user: limited\n";
        cfg << "    password: l\n";
        cfg << "    rateLimit: 5\n";
        cfg << "  - user: narrow\n";
        cfg << "    password: n\n";
        cfg << "    bandwidthLimit: 1KB\n";
        cfg << "  - user: repl\n";
        cfg << "    role: replica\n";
        cfg << "    password: p\n";
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

    /// Sends `cmds` as one batch and reads the reply until it holds one response per command.
    std::string sendBatch(const std::vector<std::string>& cmds) {
        std::string payload;
        for (const auto& cmd : cmds) {
            payload += be32(static_cast<uint32_t>(cmd.size())) + cmd;
        }
        asio::write(sock, asio::buffer(be32(static_cast<uint32_t>(payload.size())) + payload));
        std::string reply;
        while (countOf(reply, "\"code\":") < cmds.size() || !reply.ends_with('\n')) {
            char buf[65536];
            reply.append(buf, sock.read_some(asio::buffer(buf)));
        }
        return reply;
    }

    static size_t countOf(const std::string& text, const std::string& what) {
        size_t n = 0;
        for (size_t pos = text.find(what); pos != std::string::npos; pos = text.find(what, pos + what.size())) {
            n++;
        }
        return n;
    }

    /// True if the server closes the connection within `timeout`, without anything being sent to it.
    bool closedWithin(std::chrono::milliseconds timeout) {
        bool closed = false;
        char byte;
        sock.async_read_some(asio::buffer(&byte, 1), [&closed](std::error_code ec, std::size_t) { closed = ec == asio::error::eof || ec == asio::error::connection_reset; });
        ctx.restart();
        ctx.run_for(timeout);
        return closed;
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

TEST_CASE_METHOD(SessionFixture, "A reload that removes a user closes its idle connections at once", "[session]") {
    Client c;
    REQUIRE(c.send("Authorization: alice." + sha256("alicesecret")).find(codeOf(ResultType::Code::Success)) != std::string::npos);
    REQUIRE(c.send(R"(s k "v")").find(codeOf(ResultType::Code::Success)) != std::string::npos);
    writeConfig(false);
    REQUIRE(db->execCmd("r").code == ResultType::Code::Success);
    REQUIRE(c.closedWithin(std::chrono::milliseconds(1000)));
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

TEST_CASE_METHOD(SessionFixture, "Session runs the first commands of a batch that exceeds the rate limit and refuses the rest", "[session][ratelimit]") {
    Client c;
    REQUIRE(c.send("Authorization: limited." + sha256("limitedl")).find(codeOf(ResultType::Code::Success)) != std::string::npos);
    std::vector<std::string> cmds;
    for (int i = 0; i < 8; i++) {
        cmds.push_back(std::format(R"(s k{} "v")", i));
    }
    const std::string reply = c.sendBatch(cmds);
    REQUIRE(Client::countOf(reply, codeOf(ResultType::Code::Success)) == 5);
    REQUIRE(Client::countOf(reply, codeOf(ResultType::Code::RateLimited)) == 3);
    REQUIRE(reply.find(codeOf(ResultType::Code::RateLimited)) > reply.rfind(codeOf(ResultType::Code::Success)));
    REQUIRE(db->execCmd("exists k4").response == "true");
    REQUIRE(db->execCmd("exists k5").response == "false");

    REQUIRE(c.send("g k0").find(codeOf(ResultType::Code::RateLimited)) != std::string::npos);
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    REQUIRE(c.send("g k0").find(codeOf(ResultType::Code::Success)) != std::string::npos);
    REQUIRE(db->limiterOf("limited")->refusedCommands() == 4);
}

TEST_CASE_METHOD(SessionFixture, "Session shares a user's rate limit between its connections", "[session][ratelimit]") {
    Client a;
    Client b;
    for (Client* c : { &a, &b }) {
        REQUIRE(c->send("Authorization: limited." + sha256("limitedl")).find(codeOf(ResultType::Code::Success)) != std::string::npos);
    }
    const std::string first = a.sendBatch({ "g x", "g x", "g x" });
    const std::string second = b.sendBatch({ "g x", "g x", "g x" });
    REQUIRE(Client::countOf(first + second, codeOf(ResultType::Code::RateLimited)) == 1);
}

TEST_CASE_METHOD(SessionFixture, "Session drops the requests that exceed the bandwidth limit", "[session][ratelimit]") {
    Client c;
    REQUIRE(c.send("Authorization: narrow." + sha256("narrown")).find(codeOf(ResultType::Code::Success)) != std::string::npos);
    // Larger than the 1 KB/s limit, but the bucket is full: it passes and leaves a debt
    REQUIRE(c.send(std::format(R"(s big "{}")", std::string(3000, 'x'))).find(codeOf(ResultType::Code::Success)) != std::string::npos);

    // While the debt lasts, every command of a request is refused, here over several 64 KB chunks
    std::vector<std::string> cmds;
    for (int i = 0; i < 200; i++) {
        cmds.push_back(std::format(R"(s n{} "{}")", i, std::string(1000, 'y')));
    }
    const std::string reply = c.sendBatch(cmds);
    REQUIRE(Client::countOf(reply, codeOf(ResultType::Code::RateLimited)) == 200);
    REQUIRE(db->execCmd("exists n0").response == "false");

    // The session is still in step with the protocol after the dropped request
    REQUIRE(c.send("g big").find(codeOf(ResultType::Code::RateLimited)) != std::string::npos);
    REQUIRE(db->limiterOf("narrow")->refusedCommands() == 201);
}

TEST_CASE_METHOD(SessionFixture, "Session answers MalformedBatch for a dropped request whose lengths do not add up", "[session][ratelimit]") {
    Client c;
    REQUIRE(c.send("Authorization: narrow." + sha256("narrown")).find(codeOf(ResultType::Code::Success)) != std::string::npos);
    REQUIRE(c.send(std::format(R"(s big "{}")", std::string(3000, 'x'))).find(codeOf(ResultType::Code::Success)) != std::string::npos);
    const std::string payload = be32(3) + "g a" + be32(50) + "short";
    const std::string reply = c.sendRaw(be32(static_cast<uint32_t>(payload.size())) + payload);
    REQUIRE(reply.find(codeOf(ResultType::Code::RateLimited)) != std::string::npos);
}

TEST_CASE_METHOD(SessionFixture, "Session does not limit a user without limits", "[session][ratelimit]") {
    REQUIRE(db->limiterOf("bob") == nullptr);
    Client c;
    REQUIRE(c.send("Authorization: bob." + sha256("bobbob")).find(codeOf(ResultType::Code::Success)) != std::string::npos);
    const std::string reply = c.sendBatch(std::vector<std::string>(500, "g x"));
    REQUIRE(Client::countOf(reply, codeOf(ResultType::Code::RateLimited)) == 0);
}

TEST_CASE_METHOD(SessionFixture, "Deleting a user closes its idle connections at once", "[session][users]") {
    Client c;
    REQUIRE(c.send("Authorization: bob." + sha256("bobbob")).find(codeOf(ResultType::Code::Success)) != std::string::npos);
    REQUIRE(db->execCmd("user del bob", Role::Admin).code == ResultType::Code::Success);
    REQUIRE(c.closedWithin(std::chrono::milliseconds(1000)));

    Client again;
    REQUIRE(again.send("Authorization: bob." + sha256("bobbob")).find(codeOf(ResultType::Code::AuthFailed)) != std::string::npos);
}

TEST_CASE_METHOD(SessionFixture, "Changing a password closes the user's connections and only the new one is accepted", "[session][users]") {
    Client c;
    REQUIRE(c.send("Authorization: bob." + sha256("bobbob")).find(codeOf(ResultType::Code::Success)) != std::string::npos);
    REQUIRE(db->execCmd(R"(user passwd bob "n3w")", Role::Admin).code == ResultType::Code::Success);
    REQUIRE(c.closedWithin(std::chrono::milliseconds(1000)));

    Client oldPassword;
    REQUIRE(oldPassword.send("Authorization: bob." + sha256("bobbob")).find(codeOf(ResultType::Code::AuthFailed)) != std::string::npos);
    Client newPassword;
    REQUIRE(newPassword.send("Authorization: bob." + sha256("bobn3w")).find(codeOf(ResultType::Code::Success)) != std::string::npos);
}

TEST_CASE_METHOD(SessionFixture, "A revoked session finishes its current command but not the rest of the batch", "[session][users]") {
    Client c;
    REQUIRE(c.send("Authorization: root." + sha256("rootr")).find(codeOf(ResultType::Code::Success)) != std::string::npos);
    std::string payload;
    for (const std::string cmd : { R"(s before "1")", R"(user passwd root "other")", R"(s after "1")", "g before" }) {
        payload += be32(static_cast<uint32_t>(cmd.size())) + cmd;
    }
    asio::write(c.sock, asio::buffer(be32(static_cast<uint32_t>(payload.size())) + payload));
    // The server answers the commands it ran, then closes the connection
    std::string reply;
    asio::error_code ec;
    while (!ec) {
        char buf[4096];
        reply.append(buf, c.sock.read_some(asio::buffer(buf), ec));
    }
    REQUIRE(ec == asio::error::eof);
    REQUIRE(Client::countOf(reply, "\"code\":") == 3);
    REQUIRE(Client::countOf(reply, codeOf(ResultType::Code::Success)) == 2);
    REQUIRE(reply.find(codeOf(ResultType::Code::SessionRevoked)) != std::string::npos);
    REQUIRE(db->execCmd("exists after").response == "false");
}

TEST_CASE_METHOD(SessionFixture, "A new role applies to an open session from its next command", "[session][users]") {
    Client c;
    REQUIRE(c.send("Authorization: bob." + sha256("bobbob")).find(codeOf(ResultType::Code::Success)) != std::string::npos);
    REQUIRE(c.send(R"(s k "v")").find(codeOf(ResultType::Code::Success)) != std::string::npos);
    REQUIRE(db->execCmd("user role bob read", Role::Admin).code == ResultType::Code::Success);
    REQUIRE(c.send(R"(s k "w")").find(codeOf(ResultType::Code::PermissionDenied)) != std::string::npos);
    REQUIRE(c.send("g k").find(codeOf(ResultType::Code::Success)) != std::string::npos);
}

TEST_CASE_METHOD(SessionFixture, "A user added at runtime can log in", "[session][users]") {
    REQUIRE(db->execCmd(R"(user add carol "c4rol" read)", Role::Admin).code == ResultType::Code::Success);
    Client c;
    REQUIRE(c.send("Authorization: carol." + sha256("carolc4rol")).find(codeOf(ResultType::Code::Success)) != std::string::npos);
    REQUIRE(c.send(R"(s k "v")").find(codeOf(ResultType::Code::PermissionDenied)) != std::string::npos);
}

/// The JSON object a successful `replicate` answers with.
static nlohmann::json replicateReply(const std::string& reply) {
    auto outer = nlohmann::json::parse(reply);
    REQUIRE(outer["code"] == 0);
    return outer["response"];
}

TEST_CASE_METHOD(SessionFixture, "A replica gets a full sync, then a stream once it knows the history", "[session][replication]") {
    std::string historyId;
    uint64_t nextSeq = 0;
    {
        Client c;
        REQUIRE(c.send("Authorization: repl." + sha256("replp")).find(codeOf(ResultType::Code::Success)) != std::string::npos);
        auto first = replicateReply(c.send("replicate - 0"));
        REQUIRE(first["mode"] == "fullsync");
        historyId = first["historyId"];
        nextSeq = first["nextSeq"];
        REQUIRE(historyId == db->getHistoryId());
        // One handshake per session
        REQUIRE(c.send("replicate - 0").find(codeOf(ResultType::Code::PermissionDenied)) != std::string::npos);
    }
    REQUIRE(db->getReplicationLog().active());
    REQUIRE(db->execCmd(R"(s k "v")").code == ResultType::Code::Success);

    Client again;
    REQUIRE(again.send("Authorization: repl." + sha256("replp")).find(codeOf(ResultType::Code::Success)) != std::string::npos);
    auto resumed = replicateReply(again.send(std::format("replicate {} {}", historyId, nextSeq)));
    REQUIRE(resumed["mode"] == "stream");
    REQUIRE(resumed["nextSeq"] == nextSeq + 1);

    // Another history, or a position the log does not hold: full sync
    Client other;
    REQUIRE(other.send("Authorization: repl." + sha256("replp")).find(codeOf(ResultType::Code::Success)) != std::string::npos);
    REQUIRE(replicateReply(other.send(std::format("replicate {} {}", historyId, nextSeq + 100)))["mode"] == "fullsync");
}

TEST_CASE_METHOD(SessionFixture, "Only the replica role may replicate, and it may do nothing else", "[session][replication]") {
    Client bob;
    REQUIRE(bob.send("Authorization: bob." + sha256("bobbob")).find(codeOf(ResultType::Code::Success)) != std::string::npos);
    REQUIRE(bob.send("replicate - 0").find(codeOf(ResultType::Code::PermissionDenied)) != std::string::npos);

    Client repl;
    REQUIRE(repl.send("Authorization: repl." + sha256("replp")).find(codeOf(ResultType::Code::Success)) != std::string::npos);
    REQUIRE(repl.send("g k").find(codeOf(ResultType::Code::PermissionDenied)) != std::string::npos);
    REQUIRE(repl.send("replicate abc").find(codeOf(ResultType::Code::InvalidValueFormat)) != std::string::npos);
    REQUIRE(repl.send("replicate - 1 2").find(codeOf(ResultType::Code::InvalidValueFormat)) != std::string::npos);
    REQUIRE_FALSE(db->getReplicationLog().active());
}

TEST_CASE_METHOD(SessionFixture, "A replication link is not closed by the idle timeout", "[session][replication]") {
    Client repl;
    REQUIRE(repl.send("Authorization: repl." + sha256("replp")).find(codeOf(ResultType::Code::Success)) != std::string::npos);
    replicateReply(repl.send("replicate - 0"));
    REQUIRE_FALSE(repl.closedWithin(std::chrono::milliseconds(3000))); // IdleTimeout is 2 s
}
