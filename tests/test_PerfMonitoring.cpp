#include <catch2/catch_all.hpp>

#include "PerfMonitoring.hpp"

using namespace Zest;

TEST_CASE("Stats::avgLatency with requests", "[perf]") {
    Stats s;
    s.nbRequests = 10;
    s.totalLatency = 100.0;
    s.nbCacheMisses = 3;
    REQUIRE(s.avgLatency() == 10.0);
    REQUIRE(s.avgCacheMissPercent() == 30.0);
}

TEST_CASE("Stats::avgLatency with zero requests", "[perf]") {
    Stats s;
    REQUIRE(s.avgLatency() == 0.0);
    REQUIRE(s.avgCacheMissPercent() == 0.0);
}

TEST_CASE("addGetStats cache miss increments counter", "[perf]") {
    PerfMonitoring pm;
    pm.addGetStats(true, 5.0);
    pm.addGetStats(true, 3.0);
    auto j = pm.getPerformances();
    REQUIRE(j["get"]["nbRequests"] == 2);
    REQUIRE(j["get"]["nbCacheMisses"] == 2);
}

TEST_CASE("addGetStats cache hit does not increment miss", "[perf]") {
    PerfMonitoring pm;
    pm.addGetStats(false, 5.0);
    auto j = pm.getPerformances();
    REQUIRE(j["get"]["nbRequests"] == 1);
    REQUIRE(j["get"]["nbCacheMisses"] == 0);
}

TEST_CASE("addSetStats accumulation", "[perf]") {
    PerfMonitoring pm;
    pm.addSetStats(false, 10.0);
    pm.addSetStats(true, 20.0);
    auto j = pm.getPerformances();
    REQUIRE(j["set"]["nbRequests"] == 2);
    REQUIRE(j["set"]["nbCacheMisses"] == 1);
}

TEST_CASE("addDelStats accumulation", "[perf]") {
    PerfMonitoring pm;
    pm.addDelStats(false, 1.0);
    pm.addDelStats(false, 2.0);
    pm.addDelStats(true, 3.0);
    auto j = pm.getPerformances();
    REQUIRE(j["del"]["nbRequests"] == 3);
    REQUIRE(j["del"]["nbCacheMisses"] == 1);
}

TEST_CASE("getPerformances zero stats", "[perf]") {
    PerfMonitoring pm;
    auto j = pm.getPerformances();
    REQUIRE(j["total"]["nbRequests"] == 0);
    REQUIRE(j["total"]["avgCacheMissPercent"] == 0.0);
    REQUIRE(j["total"]["avgLatencyMs"] == 0.0);
}

TEST_CASE("getPerformances mixed stats aggregate correctly", "[perf]") {
    PerfMonitoring pm;
    pm.addGetStats(false, 10.0);
    pm.addSetStats(true, 20.0);
    pm.addDelStats(false, 30.0);
    auto j = pm.getPerformances();
    REQUIRE(j["total"]["nbRequests"] == 3);
    REQUIRE(j["total"]["nbCacheMisses"] == 1);
    double expectedLatency = (10.0 + 20.0 + 30.0) / 3.0;
    REQUIRE(j["total"]["avgLatencyMs"] == Catch::Approx(expectedLatency));
}

TEST_CASE("getBy/setBy/delBy stats tracked", "[perf]") {
    PerfMonitoring pm;
    pm.addGetByStats(true, 5.0);
    pm.addSetByStats(false, 10.0);
    pm.addDelByStats(false, 15.0);
    auto j = pm.getPerformances();
    REQUIRE(j["getby"]["nbRequests"] == 1);
    REQUIRE(j["setby"]["nbRequests"] == 1);
    REQUIRE(j["delby"]["nbRequests"] == 1);
}

TEST_CASE("exists/count/keys stats tracked and included in the total", "[perf]") {
    PerfMonitoring pm;
    pm.addExistsStats(false, 1.0);
    pm.addExistsStats(false, 3.0);
    pm.addCountStats(false, 4.0);
    pm.addKeysStats(false, 6.0);
    pm.addGetStats(true, 6.0);
    auto j = pm.getPerformances();
    REQUIRE(j["exists"]["nbRequests"] == 2);
    REQUIRE(j["exists"]["avgLatencyMs"] == 2.0);
    REQUIRE(j["count"]["nbRequests"] == 1);
    REQUIRE(j["keys"]["nbRequests"] == 1);
    REQUIRE(j["total"]["nbRequests"] == 5);
    REQUIRE(j["total"]["nbCacheMisses"] == 1);
    REQUIRE(j["total"]["avgLatencyMs"] == 4.0);
}

TEST_CASE("Each request lands in the latency bucket of its duration", "[perf][metrics]") {
    PerfMonitoring pm;
    pm.addGetStats(false, 0.05); // 50 us: first bucket (<= 0.1 ms)
    pm.addGetStats(false, 0.08); // 80 us: first bucket too
    pm.addGetStats(false, 3.0);  // 3 ms: the 5 ms bucket
    pm.addGetStats(false, 5000); // above every bound: the last bucket
    const Stats& get = *pm.operations()[0].second;
    REQUIRE(std::string(pm.operations()[0].first) == "get");
    REQUIRE(get.latencyBuckets[0] == 2);
    REQUIRE(get.latencyBuckets[5] == 1);
    REQUIRE(get.latencyBuckets[LATENCY_BUCKETS_SECONDS.size()] == 1);
    uint64_t total = 0;
    for (const auto& b : get.latencyBuckets) {
        total += b;
    }
    REQUIRE(total == get.nbRequests);
}
