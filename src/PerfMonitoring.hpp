#pragma once

#include "lib/json.hpp"

using json = nlohmann::json;

struct Stats {
    unsigned int nbRequests = 0;
    unsigned int nbCacheMisses = 0;

    double totalLatency = 0.0;

    double AvgLatency = 0.0;
    double AvgCacheMiss = 0.0;

    void computeAverages();
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