#include <catch2/catch_all.hpp>
#include "ZestDB.hpp"

using namespace Zest;

TEST_CASE("sha256 produces 64 hex characters", "[sha256]") {
    auto hash = sha256("hello");
    REQUIRE(hash.size() == 64);
    for (char c : hash) {
        REQUIRE((std::isxdigit(static_cast<unsigned char>(c))));
    }
}

TEST_CASE("sha256 empty string", "[sha256]") {
    auto hash = sha256("");
    REQUIRE(hash.size() == 64);
}

TEST_CASE("sha256 known value", "[sha256]") {
    auto hash = sha256("hello");
    REQUIRE(hash == "2cf24dba5fb0a30e26e83b2ac5b9e29e1b161e5c1fa7425e73043362938b9824");
}

TEST_CASE("sha256 long string", "[sha256]") {
    std::string longStr(10000, 'a');
    auto hash = sha256(longStr);
    REQUIRE(hash.size() == 64);
}

TEST_CASE("responseToJson plain string", "[utils]") {
    ResultType r{ResultType::Code::SUCCESS, "hello", 0};
    auto json = ZestDB::responseToJson(r);
    REQUIRE(json.find("\"code\":0") != std::string::npos);
    REQUIRE(json.find("\"response\":\"hello\"") != std::string::npos);
}

TEST_CASE("responseToJson JSON array", "[utils]") {
    ResultType r{ResultType::Code::SUCCESS, "[{\"k\":\"v\"}]", 1};
    auto json = ZestDB::responseToJson(r);
    REQUIRE(json.find("\"affectedRows\":1") != std::string::npos);
}

TEST_CASE("responseToJson JSON object", "[utils]") {
    ResultType r{ResultType::Code::SUCCESS, "{\"key\":\"val\"}", 0};
    auto json = ZestDB::responseToJson(r);
    REQUIRE(json.find("\"code\":0") != std::string::npos);
}

TEST_CASE("responseToJson empty response", "[utils]") {
    ResultType r{ResultType::Code::ERROR, "", 0};
    auto json = ZestDB::responseToJson(r);
    REQUIRE(json.find("\"response\":\"\"") != std::string::npos);
}

TEST_CASE("responseToJson affectedRows", "[utils]") {
    ResultType r{ResultType::Code::SUCCESS, "ok", 42};
    auto json = ZestDB::responseToJson(r);
    REQUIRE(json.find("\"affectedRows\":42") != std::string::npos);
}
