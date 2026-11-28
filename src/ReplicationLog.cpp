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

} // namespace Zest
