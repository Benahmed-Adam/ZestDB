#include <catch2/catch_all.hpp>
#include <chrono>

#include "RateLimiter.hpp"

using namespace Zest;
using namespace std::chrono_literals;

TEST_CASE("TokenBucket gives what it holds, then refills at its rate up to its burst", "[ratelimit]") {
    const auto t0 = TokenBucket::Clock::time_point{};
    TokenBucket bucket(10, 20, t0);
    REQUIRE(bucket.take(15, t0) == 15);
    REQUIRE(bucket.take(15, t0) == 5);
    REQUIRE(bucket.take(1, t0) == 0);
    REQUIRE(bucket.take(100, t0 + 500ms) == 5);
    REQUIRE(bucket.take(100, t0 + 1h) == 20);
}

TEST_CASE("TokenBucket lets a request larger than its burst through, then waits for the debt", "[ratelimit]") {
    const auto t0 = TokenBucket::Clock::time_point{};
    TokenBucket bucket(100, 100, t0);
    REQUIRE(bucket.takeWithDebt(250, t0));
    REQUIRE_FALSE(bucket.takeWithDebt(1, t0 + 1s));
    REQUIRE(bucket.takeWithDebt(1, t0 + 1600ms));
}

TEST_CASE("UserLimiter without a limit lets everything through", "[ratelimit]") {
    UserLimiter limiter(UserLimits{});
    REQUIRE(limiter.allowCommands(1'000'000) == 1'000'000);
    REQUIRE(limiter.allowBytes(1ULL << 40));
}

TEST_CASE("UserLimiter uses the rate as burst when none is set", "[ratelimit]") {
    const auto t0 = TokenBucket::Clock::time_point{};
    UserLimiter limiter(UserLimits{ 50, std::nullopt, std::nullopt }, t0);
    REQUIRE(limiter.allowCommands(80, t0) == 50);
    REQUIRE(limiter.allowBytes(1ULL << 40, t0));
}

TEST_CASE("UserLimiter keeps its buckets on reconfigure and drops a removed limit", "[ratelimit]") {
    const auto t0 = TokenBucket::Clock::time_point{};
    UserLimiter limiter(UserLimits{ 10, 10, 1000 }, t0);
    REQUIRE(limiter.allowCommands(8, t0) == 8);
    limiter.reconfigure(UserLimits{ 100, 100, 1000 }, t0);
    REQUIRE(limiter.allowCommands(100, t0) == 2);

    REQUIRE(limiter.allowBytes(5000, t0));
    REQUIRE_FALSE(limiter.allowBytes(1, t0));
    limiter.reconfigure(UserLimits{ 100, 100, std::nullopt }, t0);
    REQUIRE(limiter.allowBytes(1, t0));
}

TEST_CASE("UserLimiter counts the refused commands", "[ratelimit]") {
    UserLimiter limiter(UserLimits{ 1, 1, std::nullopt });
    limiter.addRefused(3);
    limiter.addRefused(2);
    REQUIRE(limiter.refusedCommands() == 5);
}
