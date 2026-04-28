#include <algorithm>
#include <chrono>
#include <thread>

#include "Compactor.hpp"
#include "Logger.hpp"

Compactor::Compactor(unsigned int compInter)
    : compactingInterval(compInter)
{
}

void Compactor::run(IndexManager& indexManager, StorageManager& storageManager, bool& isRunning)
{
    ZestLog(LogLevel::INFO, "Starting the compactor...");

    while (isRunning) {
        std::this_thread::sleep_for(std::chrono::seconds(this->compactingInterval));
        std::vector<IndexEntry> entries = indexManager.compact();

        if (entries.empty()) {
            ZestLog(LogLevel::DEBUG, "Compactor - index is empty, skipping segment cleanup");
            continue;
        }

        std::vector<int> usedSegmentIds;
        for (const auto& entry : entries) {
            if (!entry.isTombstone && entry.segmentId != -1) {
                if (std::find(usedSegmentIds.begin(), usedSegmentIds.end(), entry.segmentId) == usedSegmentIds.end()) {
                    usedSegmentIds.push_back(entry.segmentId);
                }
            }
        }

        if (!usedSegmentIds.empty()) {
            storageManager.removeUnusedSegments(usedSegmentIds);
        }

        ZestLog(LogLevel::DEBUG, "Compactor - compaction done, waiting...");
    }
    ZestLog(LogLevel::INFO, "Stopping the compactor...");
}