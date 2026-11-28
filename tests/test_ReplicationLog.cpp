#include <catch2/catch_all.hpp>

#include "ReplicationLog.hpp"

using namespace Zest;

TEST_CASE("ReplicationLog serves nothing until it is active", "[replication]") {
    ReplicationLog log(10);
    std::vector<ReplicationEvent> out;
    REQUIRE_FALSE(log.active());
    REQUIRE_FALSE(log.since(0, out, 100));
}

TEST_CASE("ReplicationLog serves the events from a sequence number on", "[replication]") {
    ReplicationLog log(10);
    log.activate(5);
    {
        std::lock_guard<std::mutex> lock(log.lock());
        log.push(5, "s a \"1\"");
        log.push(7, "s b \"2\""); // 6 went to a failed WAL write
        log.push(8, "d a");
    }
    std::vector<ReplicationEvent> out;
    REQUIRE(log.since(5, out, 100));
    REQUIRE(out.size() == 3);
    out.clear();
    REQUIRE(log.since(6, out, 100));
    REQUIRE(out.size() == 2);
    REQUIRE(out[0].seq == 7);
    out.clear();
    REQUIRE(log.since(7, out, 1));
    REQUIRE(out.size() == 1);
    REQUIRE(out[0].cmd == "s b \"2\"");

    // Up to date: nothing to send, but no full sync either
    out.clear();
    REQUIRE(log.since(9, out, 100));
    REQUIRE(out.empty());
    REQUIRE(log.nextSeq() == 9);

    // Before the activation, or past the last write: the replica needs a full sync
    REQUIRE_FALSE(log.since(4, out, 100));
    REQUIRE_FALSE(log.since(10, out, 100));
}

TEST_CASE("ReplicationLog drops the oldest events beyond its capacity", "[replication]") {
    ReplicationLog log(3);
    log.activate(0);
    {
        std::lock_guard<std::mutex> lock(log.lock());
        for (uint64_t seq = 0; seq < 5; seq++) {
            log.push(seq, "d k" + std::to_string(seq));
        }
    }
    std::vector<ReplicationEvent> out;
    REQUIRE_FALSE(log.since(1, out, 100));
    REQUIRE(log.since(2, out, 100));
    REQUIRE(out.size() == 3);
    REQUIRE(out.front().cmd == "d k2");
}
