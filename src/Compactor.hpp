#pragma once

#include "IndexManager.hpp"
#include "StorageManager.hpp"

class Compactor {
public:
    Compactor(unsigned int compactingInterval);
    void compactIndex(IndexManager& indexManager);
    void run(IndexManager& indexManager, StorageManager& storageManager, bool& isRunning);

private:
    unsigned int compactingInterval;
};