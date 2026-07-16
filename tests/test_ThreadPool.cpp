#include <atomic>
#include <catch2/catch_all.hpp>
#include <vector>

#include "ThreadPool.hpp"

using namespace Zest;

TEST_CASE("ThreadPool enqueue task execution", "[threadpool]") {
    ThreadPool pool(2);
    auto f = pool.enqueue([]() { return 42; });
    REQUIRE(f.get() == 42);
}

TEST_CASE("ThreadPool multiple tasks", "[threadpool]") {
    ThreadPool pool(4);
    std::vector<std::future<int>> futures;
    for (int i = 0; i < 100; i++) {
        futures.push_back(pool.enqueue([i]() { return i * 2; }));
    }
    for (size_t i = 0; i < 100; i++) {
        REQUIRE(futures[i].get() == static_cast<int>(i) * 2);
    }
}

TEST_CASE("ThreadPool waitAll blocks until complete", "[threadpool]") {
    ThreadPool pool(2);
    std::atomic<int> counter{ 0 };
    for (int i = 0; i < 10; i++) {
        pool.enqueue([&counter]() { counter.fetch_add(1); });
    }
    pool.waitAll();
    REQUIRE(counter.load() == 10);
}

TEST_CASE("ThreadPool waitAll returns immediately if empty", "[threadpool]") {
    ThreadPool pool(2);
    REQUIRE_NOTHROW(pool.waitAll());
}

TEST_CASE("ThreadPool task exception logged not crashed", "[threadpool]") {
    ThreadPool pool(1);
    auto f = pool.enqueue([]() -> int { throw std::runtime_error("test exception"); });
    pool.waitAll();
    REQUIRE_THROWS(f.get());
}

TEST_CASE("ThreadPool destructor stops threads", "[threadpool]") {
    {
        ThreadPool pool(4);
        for (int i = 0; i < 20; i++) {
            pool.enqueue([]() { std::this_thread::sleep_for(std::chrono::milliseconds(1)); });
        }
    }
    SUCCEED();
}
