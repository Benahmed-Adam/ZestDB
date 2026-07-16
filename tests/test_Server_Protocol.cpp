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

TEST_CASE("Messages constants are defined", "[server]") {
    REQUIRE(std::string(Messages::AUTH_SUCCESS).find("authenticated") != std::string::npos);
    REQUIRE(std::string(Messages::AUTH_FAILED).find("Authentication") != std::string::npos);
    REQUIRE(std::string(Messages::MALFORMED_BATCH).find("Batch") != std::string::npos);
}

TEST_CASE("ResultType SUCCESS and ERROR codes", "[server]") {
    ResultType ok{ ResultType::Code::SUCCESS, "ok", 0 };
    ResultType err{ ResultType::Code::ERROR, "fail", 0 };
    REQUIRE(ok.code == ResultType::Code::SUCCESS);
    REQUIRE(err.code == ResultType::Code::ERROR);
}

TEST_CASE("ResultType default affectedRows is zero", "[server]") {
    ResultType r;
    REQUIRE(r.affectedRows == 0);
}
