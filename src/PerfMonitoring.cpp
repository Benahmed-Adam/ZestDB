#include "PerfMonitoring.hpp"

namespace Zest {

    void Stats::computeAverages() {
        if (this->nbRequests > 0) {
            this->AvgLatency = this->totalLatency / this->nbRequests;
            this->AvgCacheMiss = (static_cast<double>(this->nbCacheMisses) / this->nbRequests) * 100.0;
        }
    }

    json PerfMonitoring::getPerformances() const {
        Stats allStats[] = { this->gStats, this->sStats, this->dStats, this->gbStats, this->sbStats, this->dbStats };

        json result = json::object();

        std::string names[] = { "get", "set", "del", "getby", "setby", "delby" };

        for (int i = 0; i < 6; ++i) {
            Stats s = allStats[i];
            s.computeAverages();

            result[names[i]] = {
                { "nbRequests", s.nbRequests },
                { "nbCacheMisses", s.nbCacheMisses },
                { "avgLatencyMs", s.AvgLatency },
                { "avgCacheMissPercent", s.AvgCacheMiss },
            };
        }

        unsigned int totalRequests = this->gStats.nbRequests + this->sStats.nbRequests + this->dStats.nbRequests + this->gbStats.nbRequests + this->sbStats.nbRequests + this->dbStats.nbRequests;
        unsigned int totalCacheMisses = this->gStats.nbCacheMisses + this->sStats.nbCacheMisses + this->dStats.nbCacheMisses + this->gbStats.nbCacheMisses + this->sbStats.nbCacheMisses + this->dbStats.nbCacheMisses;
        double totalLatency = this->gStats.totalLatency + this->sStats.totalLatency + this->dStats.totalLatency + this->gbStats.totalLatency + this->sbStats.totalLatency + this->dbStats.totalLatency;

        result["total"] = { { "nbRequests", totalRequests },
                            { "nbCacheMisses", totalCacheMisses },
                            { "avgCacheMissPercent", totalRequests > 0 ? (static_cast<double>(totalCacheMisses) / totalRequests) * 100.0 : 0.0 },
                            { "avgLatencyMs", totalRequests > 0 ? totalLatency / totalRequests : 0.0 } };

        return result;
    }

    void PerfMonitoring::addGetStats(bool isCacheMiss, double latency) {
        this->gStats.nbRequests++;
        this->gStats.totalLatency += latency;

        if (isCacheMiss) {
            this->gStats.nbCacheMisses++;
        }
    }

    void PerfMonitoring::addSetStats(bool isCacheMiss, double latency) {
        this->sStats.nbRequests++;
        this->sStats.totalLatency += latency;

        if (isCacheMiss) {
            this->sStats.nbCacheMisses++;
        }
    }

    void PerfMonitoring::addDelStats(bool isCacheMiss, double latency) {
        this->dStats.nbRequests++;
        this->dStats.totalLatency += latency;

        if (isCacheMiss) {
            this->dStats.nbCacheMisses++;
        }
    }

    void PerfMonitoring::addGetByStats(bool isCacheMiss, double latency) {
        this->gbStats.nbRequests++;
        this->gbStats.totalLatency += latency;

        if (isCacheMiss) {
            this->gbStats.nbCacheMisses++;
        }
    }

    void PerfMonitoring::addSetByStats(bool isCacheMiss, double latency) {
        this->sbStats.nbRequests++;
        this->sbStats.totalLatency += latency;

        if (isCacheMiss) {
            this->sbStats.nbCacheMisses++;
        }
    }

    void PerfMonitoring::addDelByStats(bool isCacheMiss, double latency) {
        this->dbStats.nbRequests++;
        this->dbStats.totalLatency += latency;

        if (isCacheMiss) {
            this->dbStats.nbCacheMisses++;
        }
    }

} // namespace Zest
