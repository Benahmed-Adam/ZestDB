#pragma once

#include <atomic>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

namespace Zest {

    /// One write as a replica applies it: its WAL command and sequence number.
    struct ReplicationEvent {
        uint64_t seq;
        std::string cmd;
    };

    /// The last `capacity` writes of the primary, shared by every replica sender, which each keep their own cursor.
    ///
    /// Inactive (and free) until a replica needs it. Once active, every WAL record is kept here too, in sequence
    /// order: the caller holds lock() from the numbering of the record to push().
    class ReplicationLog {
    public:
        explicit ReplicationLog(size_t maxEvents)
            : capacity(maxEvents) {
        }

        bool active() const {
            return this->isActive.load(std::memory_order_acquire);
        }

        /// Starts keeping the writes numbered from `nextSeq`. No write may be logged meanwhile (the caller holds every
        /// shard's write order lock).
        void activate(uint64_t nextSeq);

        std::mutex& lock() {
            return this->mtx;
        }

        /// Keeps one event, dropping the oldest beyond the capacity; the caller holds lock().
        void push(uint64_t seq, std::string cmd);

        /// Up to `max` events from sequence `from` on, appended to `out`.
        /// @return false if the log cannot serve `from`: older events were dropped or never kept, or `from` is past the
        ///         last numbered write. The replica then needs a full sync.
        bool since(uint64_t from, std::vector<ReplicationEvent>& out, size_t max);

        /// The sequence number of the next write.
        uint64_t nextSeq();

    private:
        const size_t capacity;
        std::atomic<bool> isActive{ false };
        std::mutex mtx;
        std::deque<ReplicationEvent> events; // in sequence order; seqs may skip a number taken by a failed WAL write
        uint64_t horizon = 0;                // every successful write numbered from here on is in `events`
        uint64_t next = 0;                   // one past the last kept event, or `horizon`
    };

} // namespace Zest
