#include "Compactor.hpp"

#include <algorithm>
#include <chrono>
#include <format>
#include <thread>

#include "Logger.hpp"

namespace Zest {

    Compactor::Compactor(const Settings &set)
        : settings(set)
    {
    }

    void Compactor::run(IndexManager &indexManager, StorageManager &storageManager, std::atomic<bool> &stopFlag)
    {
        ZestLog(LogLevel::INFO, "Starting the compactor...");

        while (!stopFlag.load()) {
            std::this_thread::sleep_for(std::chrono::seconds(this->settings.CompactingInterval));

            if (stopFlag.load()) {
                break;
            }

            std::vector<IndexEntry> entries = indexManager.compact();

            if (entries.empty()) {
                ZestLog(LogLevel::DEBUG, "Compactor - index is empty, skipping segment cleanup");
                continue;
            }

            if (stopFlag.load()) {
                break;
            }

            std::vector<int> usedSegmentIds;
            for (const auto &entry : entries) {
                if (!entry.isTombstone && entry.segmentId != -1) {
                    if (std::find(usedSegmentIds.begin(), usedSegmentIds.end(), entry.segmentId) ==
                        usedSegmentIds.end()) {
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

} // namespace Zest
