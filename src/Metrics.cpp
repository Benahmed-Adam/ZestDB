#include <filesystem>
#include <format>
#include <iterator>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <string_view>

#include "ZestDB.hpp"
#include "lib/json.hpp"

namespace Zest {

    namespace {
        /// Appends the `# HELP` and `# TYPE` lines that start a metric family.
        void family(std::string& out, std::string_view name, std::string_view type, std::string_view help) {
            std::format_to(std::back_inserter(out), "# HELP {} {}\n# TYPE {} {}\n", name, help, name, type);
        }

        /// `value` escaped for a label value: backslash, double quote and line feed.
        std::string labelValue(std::string_view value) {
            std::string out;
            for (char c : value) {
                if (c == '\\' || c == '"') {
                    out += '\\';
                    out += c;
                } else if (c == '\n') {
                    out += "\\n";
                } else {
                    out += c;
                }
            }
            return out;
        }

        template <typename T>
        void sample(std::string& out, std::string_view name, std::string_view labels, T value) {
            std::format_to(std::back_inserter(out), "{}{{{}}} {}\n", name, labels, value);
        }
    } // namespace

    ResultType ZestDB::info() const {
        namespace fs = std::filesystem;
        nlohmann::json out;

        uint64_t keys = 0;
        uint64_t reads = 0;
        uint64_t misses = 0;
        for (const auto& shard : this->shardManager->getShards()) {
            keys += shard->keyCount();
            // Only the reads of values can miss the cache: exists, count and keys read the index alone
            for (const auto& [op, stats] : shard->getPerfMonitoring().operations()) {
                if (std::string_view(op) == "get" || std::string_view(op) == "getby") {
                    reads += stats->nbRequests.load();
                    misses += stats->nbCacheMisses.load();
                }
            }
        }
        out["keys"] = keys;
        out["cacheHitRatio"] = reads > 0 ? nlohmann::json(1.0 - static_cast<double>(misses) / static_cast<double>(reads)) : nlohmann::json(nullptr);

        // Every file of DbPath: segments, indexes, WALs and scripts
        uint64_t diskBytes = 0;
        std::error_code ec;
        for (auto it = fs::recursive_directory_iterator(this->settings.DbPath, ec); !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
            std::error_code sizeEc;
            if (it->is_regular_file(sizeEc)) {
                const auto size = it->file_size(sizeEc);
                diskBytes += sizeEc ? 0 : size;
            }
        }
        out["diskBytes"] = diskBytes;

        // Archive names start with their creation date in UTC, so the greatest one is the most recent
        std::string last;
        for (auto it = fs::directory_iterator(this->settings.ArchiveStoragePath, ec); !ec && it != fs::directory_iterator(); it.increment(ec)) {
            const std::string name = it->path().filename().string();
            constexpr std::string_view prefix = "zestdb_archive_";
            if (name.starts_with(prefix) && name.ends_with(".zip") && name.size() >= prefix.size() + 19 && name > last) {
                last = name;
            }
        }
        if (last.empty()) {
            out["lastArchive"] = nullptr;
        } else {
            // zestdb_archive_2026-11-15_10-00-00.123456789.zip -> 2026-11-15T10:00:00Z
            std::string date = last.substr(15, 19);
            date[10] = 'T';
            date[13] = ':';
            date[16] = ':';
            out["lastArchive"] = { { "name", last }, { "date", date + "Z" } };
        }

        return { ResultType::Code::Success, out.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace), 1 };
    }

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

        family(out, "zestdb_rate_limited_total", "counter", "Commands refused by the rate or bandwidth limit of their user (users with a limit only).");
        {
            std::shared_lock<std::shared_mutex> usersLock(this->usersMtx);
            for (const auto& [user, limiter] : this->limiters) {
                sample(out, "zestdb_rate_limited_total", std::format(R"(user="{}")", labelValue(user)), limiter->refusedCommands());
            }
        }

        family(out, "zestdb_connections", "gauge", "Open client connections on the TCP server, authenticated or not.");
        std::format_to(std::back_inserter(out), "zestdb_connections {}\n", this->socket ? this->socket->connections() : 0);
        return out;
    }

} // namespace Zest
