#pragma once

#include <condition_variable>
#include <functional>
#include <future>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

namespace Zest {

    class ThreadPool {
    public:
        explicit ThreadPool(size_t numThreads);
        ~ThreadPool();

        template <typename F>
        std::future<typename std::result_of<F()>::type> enqueue(F&& task);

        void waitAll();

        ThreadPool(const ThreadPool&) = delete;
        ThreadPool& operator=(const ThreadPool&) = delete;
        ThreadPool(ThreadPool&&) = delete;
        ThreadPool& operator=(ThreadPool&&) = delete;

    private:
        std::vector<std::thread> workers;
        std::queue<std::function<void()>> tasks;
        std::mutex queueMutex;
        std::condition_variable condition;
        std::condition_variable completionCondition;
        bool stopFlag;
        size_t activeTasks;
    };

    template <typename F>
    std::future<typename std::result_of<F()>::type> ThreadPool::enqueue(F&& task)
    {
        using returnType = typename std::result_of<F()>::type;

        auto taskPtr = std::make_shared<std::packaged_task<returnType()>>(std::forward<F>(task));
        std::future<returnType> result = taskPtr->get_future();

        {
            std::lock_guard<std::mutex> lock(this->queueMutex);
            this->tasks.push([taskPtr = std::move(taskPtr)]() { (*taskPtr)(); });
        }

        this->condition.notify_one();
        return result;
    }

} // namespace Zest
