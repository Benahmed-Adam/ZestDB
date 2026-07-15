#pragma once

#include <atomic>

#include "IndexManager.hpp"
#include "StorageManager.hpp"

namespace Zest {

    class Compactor {
    public:
        Compactor(const Settings &set);
        void compactIndex(IndexManager &indexManager);
        void run(IndexManager &indexManager, StorageManager &storageManager, std::atomic<bool> &stopFlag);

    private:
        const Settings &settings;
    };

} // namespace Zest
