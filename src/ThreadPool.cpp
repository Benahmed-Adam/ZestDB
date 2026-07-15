#include <atomic>
#include <format>

#include "Logger.hpp"
#include "ThreadPool.hpp"

namespace Zest {

ThreadPool::ThreadPool(size_t numThreads)
    : stopFlag(false)
    , activeTasks(0)
{
    this->workers.reserve(numThreads);
    for (size_t i = 0; i < numThreads; ++i) {
        this->workers.emplace_back([this] {
            while (true) {
                std::function<void()> task;
                {
                    std::unique_lock<std::mutex> lock(this->queueMutex);
                    this->condition.wait(lock, [this] {
                        return this->stopFlag || !this->tasks.empty();
                    });

                    if (this->stopFlag && this->tasks.empty()) {
                        return;
                    }

                    if (!this->tasks.empty()) {
                        task = std::move(this->tasks.front());
                        this->tasks.pop();
                        ++this->activeTasks;
                    }
                }

                if (task) {
                    try {
                        task();
                    } catch (const std::exception& e) {
                        ZestLog(LogLevel::CRITICAL, std::format("Exception during the execution of a task in the ThreadPool : {}", e.what()));
                    }

                    {
                        std::lock_guard<std::mutex> lock(this->queueMutex);
                        --this->activeTasks;
                        if (this->activeTasks == 0 && this->tasks.empty()) {
                            this->completionCondition.notify_all();
                        }
                    }
                }
            }
        });
    }
}

void ThreadPool::waitAll()
{
    std::unique_lock<std::mutex> lock(this->queueMutex);
    this->completionCondition.wait(lock, [this] {
        return this->tasks.empty() && this->activeTasks == 0;
    });
}

ThreadPool::~ThreadPool()
{
    {
        std::lock_guard<std::mutex> lock(this->queueMutex);
        this->stopFlag = true;
    }
    this->condition.notify_all();
    for (auto& worker : this->workers) {
        worker.join();
    }
}

} // namespace Zest
