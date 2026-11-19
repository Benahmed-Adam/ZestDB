#pragma once

#include <atomic>

#include "lib/json.hpp"

namespace Zest {

    using json = nlohmann::json;

    /// Counters of one operation type. Lock-free atomics, updated by every request of the shard.
    struct Stats {
        std::atomic<unsigned int> nbRequests = 0;
        std::atomic<unsigned int> nbCacheMisses = 0;

        std::atomic<double> totalLatency = 0.0;

        Stats(const Stats&) = delete;
        Stats& operator=(const Stats&) = delete;
        Stats() = default;

        /// Mean latency in ms, 0 without requests.
        double avgLatency() const;
        /// Share of requests that missed the cache, in percent, 0 without requests.
        double avgCacheMissPercent() const;
    };

    /// Per-shard statistics of each operation type, served as JSON by the HTTP stats server.
    /// Each `add*Stats` records one request: whether it missed the cache and its latency in ms.
    class PerfMonitoring {
    public:
        /// Requests, cache misses, mean latency and cache-miss percentage of each operation type, plus a total.
        json getPerformances() const;
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
    };

} // namespace Zest
