#include <asio.hpp>
#include <catch2/catch_all.hpp>
#include <cstring>

#include "Server.hpp"
#include "Settings.hpp"

using namespace Zest;

TEST_CASE("ZestStream variant holds tcp socket", "[server]") {
    asio::io_context ctx;
    asio::ip::tcp::socket sock(ctx);
    ZestStream stream = std::move(sock);
    REQUIRE(std::holds_alternative<tcp::socket>(stream));
}

TEST_CASE("ZestStream does not hold ssl stream when tcp", "[server]") {
    asio::io_context ctx;
    asio::ip::tcp::socket sock(ctx);
    ZestStream stream = std::move(sock);
    REQUIRE_FALSE(std::holds_alternative<asio::ssl::stream<tcp::socket>>(stream));
}

TEST_CASE("4-byte big-endian encoding", "[server]") {
    uint32_t val = 0x01020304;
    unsigned char bytes[4];
    bytes[0] = (val >> 24) & 0xFF;
    bytes[1] = (val >> 16) & 0xFF;
    bytes[2] = (val >> 8) & 0xFF;
    bytes[3] = val & 0xFF;
    REQUIRE(bytes[0] == 0x01);
    REQUIRE(bytes[1] == 0x02);
    REQUIRE(bytes[2] == 0x03);
    REQUIRE(bytes[3] == 0x04);
}

TEST_CASE("ntohl converts network to host byte order", "[server]") {
    uint32_t networkOrder = htonl(0x01020304);
    uint32_t hostOrder = ntohl(networkOrder);
    REQUIRE(hostOrder == 0x01020304);
}

TEST_CASE("ResultType codes keep their protocol values", "[server]") {
    REQUIRE(static_cast<int>(ResultType::Code::Success) == 0);
    REQUIRE(static_cast<int>(ResultType::Code::UnknownCommand) == 101);
    REQUIRE(static_cast<int>(ResultType::Code::KeyNotFound) == 200);
    REQUIRE(static_cast<int>(ResultType::Code::AuthFailed) == 300);
    REQUIRE(static_cast<int>(ResultType::Code::InvalidConfigValue) == 401);
    REQUIRE(static_cast<int>(ResultType::Code::WalWriteFailed) == 503);
}

TEST_CASE("ResultType default affectedRows is zero", "[server]") {
    ResultType r;
    REQUIRE(r.affectedRows == 0);
}
