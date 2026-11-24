#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <optional>

#include "Settings.hpp"

namespace Zest {

    /// Token bucket: holds at most `burst` tokens and gains `rate` tokens per second. Time is passed in, so tests do not wait.
    class TokenBucket {
    public:
        using Clock = std::chrono::steady_clock;

        /// Starts full.
        TokenBucket(double rate, double burst, Clock::time_point now);

        /// Takes up to `wanted` whole tokens and returns how many it took (0 when the bucket is empty).
        uint64_t take(uint64_t wanted, Clock::time_point now);

        /// Takes `amount` tokens if the bucket is not empty, even more than it holds: the debt is paid back by the refill
        /// before anything else passes. A request larger than `burst` thus still goes through, at the configured average rate.
        bool takeWithDebt(uint64_t amount, Clock::time_point now);

        /// New rate and capacity; the tokens held are kept, capped at the new capacity.
        void reconfigure(double newRate, double newBurst);

    private:
        void refill(Clock::time_point now);

        double rate;
        double burst;
        double tokens;
        Clock::time_point last;
    };

    /// The limits of one user, shared by all of its sessions.
    class UserLimiter {
    public:
        explicit UserLimiter(const UserLimits& limits, TokenBucket::Clock::time_point now = TokenBucket::Clock::now());

        /// How many of the next `count` commands may run (the first ones); the others are refused.
        uint64_t allowCommands(uint64_t count, TokenBucket::Clock::time_point now = TokenBucket::Clock::now());

        /// False if the bandwidth limit refuses a request of `size` bytes.
        bool allowBytes(uint64_t size, TokenBucket::Clock::time_point now = TokenBucket::Clock::now());

        /// Applies the limits of a reloaded configuration without resetting the buckets.
        void reconfigure(const UserLimits& limits, TokenBucket::Clock::time_point now = TokenBucket::Clock::now());

        /// Records `count` refused commands.
        void addRefused(uint64_t count) {
            this->refused += count;
        }

        /// Commands refused since startup, by either limit.
        uint64_t refusedCommands() const {
            return this->refused.load();
        }

    private:
        std::mutex mtx;
        std::optional<TokenBucket> commands;
        std::optional<TokenBucket> bytes;
        std::atomic<uint64_t> refused = 0;
    };

} // namespace Zest
