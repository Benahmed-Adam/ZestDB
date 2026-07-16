#include "PerfMonitoring.hpp"

namespace Zest {

    double Stats::avgLatency() const {
        unsigned int reqs = this->nbRequests.load();
        return reqs > 0 ? this->totalLatency.load() / static_cast<double>(reqs) : 0.0;
    }

    double Stats::avgCacheMissPercent() const {
        unsigned int reqs = this->nbRequests.load();
        return reqs > 0 ? (static_cast<double>(this->nbCacheMisses.load()) / static_cast<double>(reqs)) * 100.0 : 0.0;
    }

    static json statsToJson(const Stats &s) {
        return {
            { "nbRequests", s.nbRequests.load() },
            { "nbCacheMisses", s.nbCacheMisses.load() },
            { "avgLatencyMs", s.avgLatency() },
            { "avgCacheMissPercent", s.avgCacheMissPercent() },
        };
    }

    json PerfMonitoring::getPerformances() const {
        json result = json::object();

        result["get"] = statsToJson(this->gStats);
        result["set"] = statsToJson(this->sStats);
        result["del"] = statsToJson(this->dStats);
        result["getby"] = statsToJson(this->gbStats);
        result["setby"] = statsToJson(this->sbStats);
        result["delby"] = statsToJson(this->dbStats);

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
