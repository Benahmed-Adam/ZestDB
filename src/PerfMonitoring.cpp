#include "PerfMonitoring.hpp"

void Stats::computeAverages()
{
    if (this->nbRequests > 0) {
        this->AvgLatency = this->totalLatency / this->nbRequests;
        this->AvgCacheMiss = (static_cast<double>(this->nbCacheMisses) / this->nbRequests) * 100.0;
        this->AvgSuccessfulRequests = (static_cast<double>(this->nbSuccessfulRequests) / this->nbRequests) * 100.0;
    }
}

json PerfMonitoring::getPerformances() const
{
    Stats allStats[] = { this->gStats, this->sStats, this->dStats, this->gbStats, this->sbStats, this->dbStats };

    json result = json::object();

    std::string names[] = { "get", "set", "del", "getby", "setby", "delby" };

    for (int i = 0; i < 6; ++i) {
        Stats s = allStats[i];
        s.computeAverages();

        result[names[i]] = {
            { "nbRequests", s.nbRequests },
            { "nbCacheMisses", s.nbCacheMisses },
            { "nbSuccessfulRequests", s.nbSuccessfulRequests },
            { "nbUnsuccessfulRequests", s.nbUnsuccessfulRequests },
            { "avgLatencyMs", s.AvgLatency },
            { "avgCacheMissPercent", s.AvgCacheMiss },
            { "avgSuccessPercent", s.AvgSuccessfulRequests }
        };
    }

    unsigned int totalRequests = this->gStats.nbRequests + this->sStats.nbRequests + this->dStats.nbRequests + this->gbStats.nbRequests + this->sbStats.nbRequests + this->dbStats.nbRequests;
    unsigned int totalCacheMisses = this->gStats.nbCacheMisses + this->sStats.nbCacheMisses + this->dStats.nbCacheMisses + this->gbStats.nbCacheMisses + this->sbStats.nbCacheMisses + this->dbStats.nbCacheMisses;
    unsigned int totalSuccess = this->gStats.nbSuccessfulRequests + this->sStats.nbSuccessfulRequests + this->dStats.nbSuccessfulRequests + this->gbStats.nbSuccessfulRequests + this->sbStats.nbSuccessfulRequests + this->dbStats.nbSuccessfulRequests;
    double totalLatency = this->gStats.totalLatency + this->sStats.totalLatency + this->dStats.totalLatency + this->gbStats.totalLatency + this->sbStats.totalLatency + this->dbStats.totalLatency;

    result["total"] = {
        { "nbRequests", totalRequests },
        { "nbCacheMisses", totalCacheMisses },
        { "nbSuccessfulRequests", totalSuccess },
        { "avgCacheMissPercent", totalRequests > 0 ? (static_cast<double>(totalCacheMisses) / totalRequests) * 100.0 : 0.0 },
        { "avgSuccessPercent", totalRequests > 0 ? (static_cast<double>(totalSuccess) / totalRequests) * 100.0 : 0.0 },
        { "avgLatencyMs", totalRequests > 0 ? totalLatency / totalRequests : 0.0 }
    };

    return result;
}

void PerfMonitoring::addGetStats(bool isSuccess, bool isCacheMiss, double latency)
{
    this->gStats.nbRequests++;
    this->gStats.totalLatency += latency;

    if (isSuccess) {
        this->gStats.nbSuccessfulRequests++;
    } else {
        this->gStats.nbUnsuccessfulRequests++;
    }

    if (isCacheMiss) {
        this->gStats.nbCacheMisses++;
    }
}

void PerfMonitoring::addSetStats(bool isSuccess, bool isCacheMiss, double latency)
{
    this->sStats.nbRequests++;
    this->sStats.totalLatency += latency;

    if (isSuccess) {
        this->sStats.nbSuccessfulRequests++;
    } else {
        this->sStats.nbUnsuccessfulRequests++;
    }

    if (isCacheMiss) {
        this->sStats.nbCacheMisses++;
    }
}

void PerfMonitoring::addDelStats(bool isSuccess, bool isCacheMiss, double latency)
{
    this->dStats.nbRequests++;
    this->dStats.totalLatency += latency;

    if (isSuccess) {
        this->dStats.nbSuccessfulRequests++;
    } else {
        this->dStats.nbUnsuccessfulRequests++;
    }

    if (isCacheMiss) {
        this->dStats.nbCacheMisses++;
    }
}

void PerfMonitoring::addGetByStats(bool isSuccess, bool isCacheMiss, double latency)
{
    this->gbStats.nbRequests++;
    this->gbStats.totalLatency += latency;

    if (isSuccess) {
        this->gbStats.nbSuccessfulRequests++;
    } else {
        this->gbStats.nbUnsuccessfulRequests++;
    }

    if (isCacheMiss) {
        this->gbStats.nbCacheMisses++;
    }
}

void PerfMonitoring::addSetByStats(bool isSuccess, bool isCacheMiss, double latency)
{
    this->sbStats.nbRequests++;
    this->sbStats.totalLatency += latency;

    if (isSuccess) {
        this->sbStats.nbSuccessfulRequests++;
    } else {
        this->sbStats.nbUnsuccessfulRequests++;
    }

    if (isCacheMiss) {
        this->sbStats.nbCacheMisses++;
    }
}

void PerfMonitoring::addDelByStats(bool isSuccess, bool isCacheMiss, double latency)
{
    this->dbStats.nbRequests++;
    this->dbStats.totalLatency += latency;

    if (isSuccess) {
        this->dbStats.nbSuccessfulRequests++;
    } else {
        this->dbStats.nbUnsuccessfulRequests++;
    }

    if (isCacheMiss) {
        this->dbStats.nbCacheMisses++;
    }
}