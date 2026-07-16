#pragma once

#include <atomic>

#include "lib/json.hpp"

namespace Zest {

    using json = nlohmann::json;

    struct Stats {
        std::atomic<unsigned int> nbRequests = 0;
        std::atomic<unsigned int> nbCacheMisses = 0;

        std::atomic<double> totalLatency = 0.0;

        Stats(const Stats &) = delete;
        Stats &operator=(const Stats &) = delete;
        Stats() = default;

        double avgLatency() const;
        double avgCacheMissPercent() const;
    };

    class PerfMonitoring {
    public:
        json getPerformances() const;
        void addGetStats(bool isCacheMiss, double latency);
        void addSetStats(bool isCacheMiss, double latency);
        void addDelStats(bool isCacheMiss, double latency);
        void addGetByStats(bool isCacheMiss, double latency);
        void addSetByStats(bool isCacheMiss, double latency);
        void addDelByStats(bool isCacheMiss, double latency);

    private:
        Stats gStats;
        Stats sStats;
        Stats dStats;
        Stats gbStats;
        Stats sbStats;
        Stats dbStats;
    };

} // namespace Zest
