#pragma once

#include "IndexManager.hpp"

class Compactor {
public:
    Compactor(unsigned int compactingInterval);
    void compactIndex(IndexManager& indexManager);
    void run(IndexManager& indexManager, bool& isRunning);
private:
    unsigned int compactingInterval;
};