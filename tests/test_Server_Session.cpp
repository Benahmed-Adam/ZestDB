#include <asio.hpp>
#include <catch2/catch_all.hpp>
#include <filesystem>
#include <fstream>
#include <thread>

#include "ZestDB.hpp"

using namespace Zest;
namespace fs = std::filesystem;

static constexpr unsigned short TEST_DB_PORT = 29521;

class SessionFixture {
public:
    fs::path testDir;
    fs::path originalCwd;
    std::unique_ptr<ZestDB> db;
    std::thread ioThread;

    SessionFixture() {
        originalCwd = fs::current_path();
        testDir = fs::temp_directory_path() / ("zestdb_test_session_" + std::to_string(reinterpret_cast<uintptr_t>(this)));
        fs::create_directories(testDir);

        std::ofstream cfg(testDir / "config.yaml");
        cfg << "DbPath: " << (testDir / "db").string() << "\n";
        cfg << "MaxKeySize: 255\n";
        cfg << "DBPort: " << TEST_DB_PORT << "\n";
        cfg << "WebPort: 29522\n";
        cfg << "ArchiveStoragePath: " << (testDir / "archive").string() << "\n";
        cfg << "AutoArchiveSaving: false\n";
        cfg << "users:\n";
        cfg << "  - user: alice\n";
        cfg << "    tokenHash: " << sha256(sha256("alicesecret")) << "\n";
        cfg << "  - user: bob\n";
        cfg << "    password: bob\n";
        cfg.close();

        fs::current_path(testDir);
        db = std::make_unique<ZestDB>();
        ioThread = std::thread([this] { db->ioCtx.run(); });
    }

    ~SessionFixture() {
        db->stop();
        ioThread.join();
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
        asio::write(sock, asio::buffer(bytes));
        asio::streambuf buf;
        asio::error_code ec;
        asio::read_until(sock, buf, '\n', ec);
        return std::string(asio::buffers_begin(buf.data()), asio::buffers_end(buf.data()));
    }

    std::string send(const std::string& cmd) {
        std::string payload = be32(static_cast<uint32_t>(cmd.size())) + cmd;
        return sendRaw(be32(static_cast<uint32_t>(payload.size())) + payload);
    }
};

TEST_CASE_METHOD(SessionFixture, "Session authenticates with a tokenHash user", "[session]") {
    Client c;
    REQUIRE(c.send("Authorization: alice." + sha256("alicesecret")).find(Messages::AUTH_SUCCESS) != std::string::npos);
    REQUIRE(c.send("s key1 value1").find(Messages::SUCCESS_SET) != std::string::npos);
}

TEST_CASE_METHOD(SessionFixture, "Session still accepts a deprecated plaintext password user", "[session]") {
    Client c;
    REQUIRE(c.send("Authorization: bob." + sha256("bobbob")).find(Messages::AUTH_SUCCESS) != std::string::npos);
}

TEST_CASE_METHOD(SessionFixture, "Session refuses a wrong token or the stored hash itself", "[session]") {
    Client wrong;
    REQUIRE(wrong.send("Authorization: alice.deadbeef").find(Messages::AUTH_FAILED) != std::string::npos);
    Client storedHash;
    REQUIRE(storedHash.send("Authorization: alice." + sha256(sha256("alicesecret"))).find(Messages::AUTH_FAILED) != std::string::npos);
}

TEST_CASE_METHOD(SessionFixture, "Session refuses an oversized payload before authentication", "[session]") {
    Client c;
    REQUIRE(c.sendRaw(be32(0xFFFFFFFF)).find(Messages::PAYLOAD_TOO_LARGE) != std::string::npos);
}
