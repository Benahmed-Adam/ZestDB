#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <span>
#include <utility>

#include "lib/json.hpp"

namespace Zest {

    using json = nlohmann::json;

    /// Upper bounds, in seconds, of the latency histogram buckets (the Prometheus `le` labels); a last bucket takes the rest.
    inline constexpr std::array<double, 13> LATENCY_BUCKETS_SECONDS = { 0.0001, 0.00025, 0.0005, 0.001, 0.0025, 0.005, 0.01, 0.025, 0.05, 0.1, 0.25, 0.5, 1.0 };

    /// Counters of one operation type. Lock-free atomics, updated by every request of the shard.
    struct Stats {
        std::atomic<uint64_t> nbRequests = 0;
        std::atomic<uint64_t> nbCacheMisses = 0;

        std::atomic<double> totalLatency = 0.0; // in ms

        /// Requests per latency bucket, not cumulative: `latencyBuckets[i]` counts the requests above the bound i-1 and
        /// up to the bound i of LATENCY_BUCKETS_SECONDS, the last one those above every bound.
        std::array<std::atomic<uint64_t>, LATENCY_BUCKETS_SECONDS.size() + 1> latencyBuckets{};

        Stats(const Stats&) = delete;
        Stats& operator=(const Stats&) = delete;
        Stats() = default;

        /// Mean latency in ms, 0 without requests.
        double avgLatency() const;
        /// Share of requests that missed the cache, in percent, 0 without requests.
        double avgCacheMissPercent() const;
    };

    /// Per-shard statistics of each operation type, served as JSON by the HTTP stats server and as Prometheus metrics.
    /// Each `add*Stats` records one request: whether it missed the cache and its latency in ms.
    class PerfMonitoring {
    public:
        /// Requests, cache misses, mean latency and cache-miss percentage of each operation type, plus a total.
        json getPerformances() const;

        /// Every operation type with its name (`get`, `set`, ...), in a fixed order.
        std::span<const std::pair<const char*, const Stats*>> operations() const {
            return this->all;
        }

        void addGetStats(bool isCacheMiss, double latency);
        void addSetStats(bool isCacheMiss, double latency);
        void addDelStats(bool isCacheMiss, double latency);
        void addGetByStats(bool isCacheMiss, double latency);
        void addSetByStats(bool isCacheMiss, double latency);
        void addDelByStats(bool isCacheMiss, double latency);
        void addExistsStats(bool isCacheMiss, double latency);
        void addCountStats(bool isCacheMiss, double latency);
        void addKeysStats(bool isCacheMiss, double latency);

    private:
        /// Adds one request to `stats`.
        static void record(Stats& stats, bool isCacheMiss, double latency);

        Stats gStats;
        Stats sStats;
        Stats dStats;
        Stats gbStats;
        Stats sbStats;
        Stats dbStats;
        Stats eStats;
        Stats cStats;
        Stats kStats;

        const std::array<std::pair<const char*, const Stats*>, 9> all = { {
            { "get", &gStats },
            { "set", &sStats },
            { "del", &dStats },
            { "getby", &gbStats },
            { "setby", &sbStats },
            { "delby", &dbStats },
            { "exists", &eStats },
            { "count", &cStats },
            { "keys", &kStats },
        } };
    };

} // namespace Zest
