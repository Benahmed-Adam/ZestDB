#include "PerfMonitoring.hpp"

#include <algorithm>

namespace Zest {

    double Stats::avgLatency() const {
        const uint64_t reqs = this->nbRequests.load();
        return reqs > 0 ? this->totalLatency.load() / static_cast<double>(reqs) : 0.0;
    }

    double Stats::avgCacheMissPercent() const {
        const uint64_t reqs = this->nbRequests.load();
        return reqs > 0 ? (static_cast<double>(this->nbCacheMisses.load()) / static_cast<double>(reqs)) * 100.0 : 0.0;
    }

    static json statsToJson(const Stats& s) {
        return {
            { "nbRequests", s.nbRequests.load() },
            { "nbCacheMisses", s.nbCacheMisses.load() },
            { "avgLatencyMs", s.avgLatency() },
            { "avgCacheMissPercent", s.avgCacheMissPercent() },
        };
    }

    json PerfMonitoring::getPerformances() const {
        json result = json::object();

        uint64_t totalRequests = 0;
        uint64_t totalCacheMisses = 0;
        double totalLatency = 0.0;
        for (const auto& [name, stats] : this->all) {
            result[name] = statsToJson(*stats);
            totalRequests += stats->nbRequests;
            totalCacheMisses += stats->nbCacheMisses;
            totalLatency += stats->totalLatency;
        }

        result["total"] = { { "nbRequests", totalRequests },
                            { "nbCacheMisses", totalCacheMisses },
                            { "avgCacheMissPercent", totalRequests > 0 ? (static_cast<double>(totalCacheMisses) / static_cast<double>(totalRequests)) * 100.0 : 0.0 },
                            { "avgLatencyMs", totalRequests > 0 ? totalLatency / static_cast<double>(totalRequests) : 0.0 } };

        return result;
    }

    void PerfMonitoring::record(Stats& stats, bool isCacheMiss, double latency) {
        stats.nbRequests++;
        stats.totalLatency += latency;
        const double seconds = latency / 1000.0;
        const auto bucket = static_cast<size_t>(std::lower_bound(LATENCY_BUCKETS_SECONDS.begin(), LATENCY_BUCKETS_SECONDS.end(), seconds) - LATENCY_BUCKETS_SECONDS.begin());
        stats.latencyBuckets[bucket]++;

        if (isCacheMiss) {
            stats.nbCacheMisses++;
        }
    }

    void PerfMonitoring::addGetStats(bool isCacheMiss, double latency) {
        record(this->gStats, isCacheMiss, latency);
    }

    void PerfMonitoring::addSetStats(bool isCacheMiss, double latency) {
        record(this->sStats, isCacheMiss, latency);
    }

    void PerfMonitoring::addDelStats(bool isCacheMiss, double latency) {
        record(this->dStats, isCacheMiss, latency);
    }

    void PerfMonitoring::addGetByStats(bool isCacheMiss, double latency) {
        record(this->gbStats, isCacheMiss, latency);
    }

    void PerfMonitoring::addSetByStats(bool isCacheMiss, double latency) {
        record(this->sbStats, isCacheMiss, latency);
    }

    void PerfMonitoring::addDelByStats(bool isCacheMiss, double latency) {
        record(this->dbStats, isCacheMiss, latency);
    }

    void PerfMonitoring::addExistsStats(bool isCacheMiss, double latency) {
        record(this->eStats, isCacheMiss, latency);
    }

    void PerfMonitoring::addCountStats(bool isCacheMiss, double latency) {
        record(this->cStats, isCacheMiss, latency);
    }

    void PerfMonitoring::addKeysStats(bool isCacheMiss, double latency) {
        record(this->kStats, isCacheMiss, latency);
    }

} // namespace Zest
