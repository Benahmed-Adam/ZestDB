#include "RateLimiter.hpp"

#include <algorithm>
#include <cmath>

namespace Zest {

    TokenBucket::TokenBucket(double rate, double burst, Clock::time_point now)
        : rate(rate),
          burst(burst),
          tokens(burst),
          last(now) {
    }

    void TokenBucket::refill(Clock::time_point now) {
        if (now > this->last) {
            const double elapsed = std::chrono::duration<double>(now - this->last).count();
            this->tokens = std::min(this->burst, this->tokens + elapsed * this->rate);
            this->last = now;
        }
    }

    uint64_t TokenBucket::take(uint64_t wanted, Clock::time_point now) {
        this->refill(now);
        const auto available = static_cast<uint64_t>(std::max(0.0, std::floor(this->tokens)));
        const uint64_t taken = std::min(wanted, available);
        this->tokens -= static_cast<double>(taken);
        return taken;
    }

    bool TokenBucket::takeWithDebt(uint64_t amount, Clock::time_point now) {
        this->refill(now);
        if (this->tokens <= 0.0) {
            return false;
        }
        this->tokens -= static_cast<double>(amount);
        return true;
    }

    void TokenBucket::reconfigure(double newRate, double newBurst) {
        this->rate = newRate;
        this->burst = newBurst;
        this->tokens = std::min(this->tokens, newBurst);
    }

    UserLimiter::UserLimiter(const UserLimits& limits, TokenBucket::Clock::time_point now) {
        this->reconfigure(limits, now);
    }

    void UserLimiter::reconfigure(const UserLimits& limits, TokenBucket::Clock::time_point now) {
        std::lock_guard<std::mutex> lock(this->mtx);
        const auto apply = [now](std::optional<TokenBucket>& bucket, std::optional<double> rate, double burst) {
            if (!rate) {
                bucket.reset();
            } else if (bucket) {
                bucket->reconfigure(*rate, burst);
            } else {
                bucket.emplace(*rate, burst, now);
            }
        };
        apply(this->commands, limits.rateLimit, limits.burst.value_or(limits.rateLimit.value_or(0)));
        const std::optional<double> bandwidth = limits.bandwidthLimit ? std::optional<double>(static_cast<double>(*limits.bandwidthLimit)) : std::nullopt;
        apply(this->bytes, bandwidth, bandwidth.value_or(0));
    }

    uint64_t UserLimiter::allowCommands(uint64_t count, TokenBucket::Clock::time_point now) {
        std::lock_guard<std::mutex> lock(this->mtx);
        return this->commands ? this->commands->take(count, now) : count;
    }

    bool UserLimiter::allowBytes(uint64_t size, TokenBucket::Clock::time_point now) {
        std::lock_guard<std::mutex> lock(this->mtx);
        return !this->bytes || this->bytes->takeWithDebt(size, now);
    }

} // namespace Zest
