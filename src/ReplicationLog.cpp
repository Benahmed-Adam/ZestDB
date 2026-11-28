#include "ReplicationLog.hpp"

#include <algorithm>

namespace Zest {

    void ReplicationLog::activate(uint64_t nextSeq) {
        std::lock_guard<std::mutex> lock(this->mtx);
        if (this->isActive.load()) {
            return;
        }
        this->horizon = nextSeq;
        this->next = nextSeq;
        this->isActive.store(true, std::memory_order_release);
    }

    void ReplicationLog::push(uint64_t seq, std::string cmd) {
        // shortcut: bounded in events, not bytes; with large values `capacity` events can take a lot of memory
        this->events.push_back({ seq, std::move(cmd) });
        this->next = seq + 1;
        if (this->events.size() > this->capacity) {
            this->horizon = this->events.front().seq + 1;
            this->events.pop_front();
        }
    }

    bool ReplicationLog::since(uint64_t from, std::vector<ReplicationEvent>& out, size_t max) {
        std::lock_guard<std::mutex> lock(this->mtx);
        if (!this->isActive.load() || from < this->horizon || from > this->next) {
            return false;
        }
        auto it = std::ranges::lower_bound(this->events, from, {}, &ReplicationEvent::seq);
        for (; it != this->events.end() && max > 0; ++it, --max) {
            out.push_back(*it);
        }
        return true;
    }

    uint64_t ReplicationLog::nextSeq() {
        std::lock_guard<std::mutex> lock(this->mtx);
        return this->next;
    }

    uint64_t ReplicationLog::subscribe(std::function<void()> wake) {
        std::lock_guard<std::mutex> lock(this->subscribersMtx);
        const uint64_t id = this->nextSubscriber++;
        this->subscribers.emplace(id, std::move(wake));
        return id;
    }

    void ReplicationLog::unsubscribe(uint64_t id) {
        std::lock_guard<std::mutex> lock(this->subscribersMtx);
        this->subscribers.erase(id);
    }

    void ReplicationLog::notify() {
        // Called outside the lock: a waker may drop the last reference to a session, whose destructor unsubscribes
        thread_local std::vector<std::function<void()>> wakers;
        {
            std::lock_guard<std::mutex> lock(this->subscribersMtx);
            for (const auto& [id, wake] : this->subscribers) {
                wakers.push_back(wake);
            }
        }
        for (const auto& wake : wakers) {
            wake();
        }
        wakers.clear();
    }

} // namespace Zest
