#include <thread>
#include <chrono>

#include "Compactor.hpp"
#include "Logger.hpp"

Compactor::Compactor(unsigned int compInter): compactingInterval(compInter) {}

void Compactor::run(IndexManager& indexManager, bool& isRunning) {
    ZestLog(LogLevel::INFO, "Starting the compactor...");
    while (isRunning) {
        indexManager.compact();
        std::this_thread::sleep_for(std::chrono::seconds(this->compactingInterval));
    }
    ZestLog(LogLevel::INFO, "Stopping the compator...");
}