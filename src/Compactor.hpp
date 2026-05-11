#pragma once

#include <atomic>

#include "IndexManager.hpp"
#include "StorageManager.hpp"

class Compactor {
public:
    Compactor(const Settings& set);
    void compactIndex(IndexManager& indexManager);
    void run(IndexManager& indexManager, StorageManager& storageManager, std::atomic<bool>& stopFlag);

private:
    unsigned int compactingInterval;
};