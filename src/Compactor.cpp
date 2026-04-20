#include <thread>
#include <chrono>

#include "Compactor.hpp"
#include "Logger.hpp"

Compactor::Compactor(unsigned int compInter): compactingInterval(compInter) {}

void Compactor::run(IndexManager& indexManager, bool& isRunning) {
    ZestLog(LogLevel::INFO, "Starting the compactor...");
    while (isRunning) {
        this->compactIndex(indexManager);
        std::this_thread::sleep_for(std::chrono::seconds(10));
    }
    ZestLog(LogLevel::INFO, "Stopping the compator...");
}

void Compactor::compactIndex(IndexManager& indexManager) {
    ZestLog(LogLevel::INFO, "Compacting the INDEX file...");
    indexManager.isCompacting = true;
    // TODO Parcourir tout l'index pour voir si la flag isTombstone est vrai et si oui alors dans le b+tree on n'inclue pas ce noeud etc....
    indexManager.isCompacting = false;
    ZestLog(LogLevel::INFO, "The INDEX file is compacted");
}