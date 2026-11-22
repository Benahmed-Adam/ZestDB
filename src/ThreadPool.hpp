#pragma once

#include <condition_variable>
#include <functional>
#include <future>
#include <mutex>
#include <queue>
#include <thread>
#include <type_traits>
#include <vector>

namespace Zest {

    /// A fixed set of worker threads running queued tasks, used to run one task per shard in parallel.
    class ThreadPool {
    public:
        /// Starts `numThreads` workers.
        explicit ThreadPool(size_t numThreads);
        /// Lets the workers finish every queued task, then joins them.
        ~ThreadPool();

        /// Queues `task` and returns a future for its result. An exception thrown by the task is rethrown by `future.get()`.
        template <typename F>
        std::future<std::invoke_result_t<F>> enqueue(F&& task);

        /// Blocks until the queue is empty and no task is running.
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
        size_t activeTasks; // tasks taken from the queue and still running
    };

    template <typename F>
    std::future<std::invoke_result_t<F>> ThreadPool::enqueue(F&& task) {
        using returnType = std::invoke_result_t<F>;

        // std::function needs a copyable callable and packaged_task is move-only: share it through a shared_ptr
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
