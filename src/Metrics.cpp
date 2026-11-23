#include <format>
#include <iterator>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <string_view>

#include "ZestDB.hpp"

namespace Zest {

    namespace {
        /// Appends the `# HELP` and `# TYPE` lines that start a metric family.
        void family(std::string& out, std::string_view name, std::string_view type, std::string_view help) {
            std::format_to(std::back_inserter(out), "# HELP {} {}\n# TYPE {} {}\n", name, help, name, type);
        }

        template <typename T>
        void sample(std::string& out, std::string_view name, std::string_view labels, T value) {
            std::format_to(std::back_inserter(out), "{}{{{}}} {}\n", name, labels, value);
        }
    } // namespace

    std::string ZestDB::prometheusMetrics() {
        std::shared_lock<std::shared_mutex> lock(this->writeMtx);
        const auto& shards = this->shardManager->getShards();
        std::string out;

        const auto perOperation = [&](std::string_view name, std::string_view type, std::string_view help, auto valueOf) {
            family(out, name, type, help);
            for (size_t i = 0; i < shards.size(); i++) {
                for (const auto& [op, stats] : shards[i]->getPerfMonitoring().operations()) {
                    sample(out, name, std::format(R"(shard="{}",op="{}")", i, op), valueOf(*stats));
                }
            }
        };
        perOperation("zestdb_requests_total", "counter", "Requests handled, per shard and operation.", [](const Stats& s) { return s.nbRequests.load(); });
        perOperation("zestdb_cache_misses_total", "counter", "Requests whose value was not in the cache and was read from disk.", [](const Stats& s) { return s.nbCacheMisses.load(); });
        perOperation("zestdb_request_latency_avg_seconds", "gauge", "Mean request latency since startup (avgLatencyMs of the JSON stats, in seconds).", [](const Stats& s) { return s.avgLatency() / 1000.0; });
        perOperation("zestdb_cache_miss_ratio", "gauge", "Share of requests that missed the cache since startup (avgCacheMissPercent of the JSON stats, from 0 to 1).", [](const Stats& s) { return s.avgCacheMissPercent() / 100.0; });

        family(out, "zestdb_request_duration_seconds", "histogram", "Request latency, per shard and operation.");
        for (size_t i = 0; i < shards.size(); i++) {
            for (const auto& [op, stats] : shards[i]->getPerfMonitoring().operations()) {
                const std::string labels = std::format(R"(shard="{}",op="{}")", i, op);
                uint64_t cumulative = 0;
                for (size_t b = 0; b < stats->latencyBuckets.size(); b++) {
                    cumulative += stats->latencyBuckets[b].load();
                    const std::string le = b < LATENCY_BUCKETS_SECONDS.size() ? std::format("{}", LATENCY_BUCKETS_SECONDS[b]) : "+Inf";
                    sample(out, "zestdb_request_duration_seconds_bucket", std::format(R"({},le="{}")", labels, le), cumulative);
                }
                sample(out, "zestdb_request_duration_seconds_sum", labels, stats->totalLatency.load() / 1000.0);
                // The +Inf bucket rather than nbRequests: the two may differ by a request recorded meanwhile
                sample(out, "zestdb_request_duration_seconds_count", labels, cumulative);
            }
        }

        const auto perShard = [&](std::string_view name, std::string_view type, std::string_view help, auto valueOf) {
            family(out, name, type, help);
            for (size_t i = 0; i < shards.size(); i++) {
                sample(out, name, std::format(R"(shard="{}")", i), valueOf(*shards[i]));
            }
        };
        perShard("zestdb_keys", "gauge", "Keys stored; expired keys count until the next compaction drops them.", [](Shard& s) { return s.keyCount(); });
        perShard("zestdb_live_bytes", "gauge", "Bytes of segment data the live keys point to.", [](Shard& s) { return s.liveBytes(); });
        perShard("zestdb_stored_bytes", "gauge", "Bytes written to the data segments, live or dead.", [](Shard& s) { return s.storedBytes(); });
        perShard("zestdb_cache_entries", "gauge", "Keys in the LRU cache.", [](Shard& s) { return s.cacheEntries(); });
        perShard("zestdb_cache_capacity", "gauge", "Maximum number of keys in the LRU cache (CacheSize).", [](Shard& s) { return s.cacheCapacity(); });
        perShard("zestdb_compactions_total", "counter", "Compactions run since startup.", [](const Shard& s) { return s.compactions(); });

        family(out, "zestdb_connections", "gauge", "Open client connections on the TCP server, authenticated or not.");
        std::format_to(std::back_inserter(out), "zestdb_connections {}\n", this->socket ? this->socket->connections() : 0);
        return out;
    }

} // namespace Zest
