#include "ThreadPool.hpp"
#include <atomic>

ThreadPool::ThreadPool(size_t numThreads)
    : stopFlag(false)
    , activeTasks(0)
{
    workers.reserve(numThreads);
    for (size_t i = 0; i < numThreads; ++i) {
        workers.emplace_back([this] {
            while (true) {
                std::function<void()> task;
                {
                    std::unique_lock<std::mutex> lock(queueMutex);
                    condition.wait(lock, [this] {
                        return stopFlag || !tasks.empty();
                    });

                    if (stopFlag && tasks.empty()) {
                        return;
                    }

                    if (!tasks.empty()) {
                        task = std::move(tasks.front());
                        tasks.pop();
                        ++activeTasks;
                    }
                }

                if (task) {
                    task();
                    {
                        std::lock_guard<std::mutex> lock(queueMutex);
                        --activeTasks;
                        if (activeTasks == 0 && tasks.empty()) {
                            completionCondition.notify_all();
                        }
                    }
                }
            }
        });
    }
}

void ThreadPool::waitAll()
{
    std::unique_lock<std::mutex> lock(queueMutex);
    completionCondition.wait(lock, [this] {
        return tasks.empty() && activeTasks == 0;
    });
}

ThreadPool::~ThreadPool()
{
    {
        std::lock_guard<std::mutex> lock(queueMutex);
        stopFlag = true;
    }
    condition.notify_all();
    for (auto& worker : workers) {
        worker.join();
    }
}