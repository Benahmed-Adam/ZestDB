# Operations

## Running the server

```bash
make run            # or ./bin/zestdb
./bin/zestdb pop    # populate a million keys on startup (demo)
```

The process also starts the HTTP monitoring server and an interactive REPL. Type `q` / `quit` to exit gracefully.

## Stopping

`q` in the REPL, `Ctrl+C` (`SIGINT`) and `SIGTERM` all stop the server gracefully:

1. The server stops accepting connections.
2. Idle sessions are closed at once.
3. A session processing a request finishes it, runs every command of the batch and sends the reply, then closes. Sessions still busy after 5 seconds are closed without their reply.
4. Every shard is flushed to disk.

A client that loses its connection without a reply cannot know whether its last request was applied. The writes of a request that ran are in the WAL either way, so retry it only if it is safe to apply twice.

## Docker

An image is published on Docker Hub as `benahmedadam/zestdb`: `latest` and `sha-<commit>` for each merge into `main`, `dev` and `dev-sha-<commit>` for each merge into `dev`. To build it locally: `docker build -t zestdb .` The Docker Hub page is [`docker/README.md`](../docker/README.md): update it with the image.
```bash
docker run -it -p 7321:7321 -p 1237:1237 \
  -v zestdb-data:/data/db -v zestdb-archive:/data/archive \
  -v "$(pwd)/config.yaml:/data/config.yaml" \
  benahmedadam/zestdb:latest
```
- `-it` connects the interactive REPL; without it the server still runs, but nothing reads the commands typed in the terminal. Use `-dit` and `docker attach` to run it in the background and reach the REPL later (`Ctrl+P` `Ctrl+Q` detaches).
- `/data/db` and `/data/archive` are volumes: name them as above, otherwise Docker creates anonymous volumes that a new `docker run` does not reuse.
- The default configuration (`docker/config.yaml`) accepts every address and has **no user account**, so no client can connect over TCP until you mount your own `config.yaml` over `/data/config.yaml` with users. The REPL works without one.
- `docker stop` sends `SIGTERM`, which stops the server gracefully (see [stopping](#stopping)). Docker kills the container after 10 seconds: keep that default or raise it with `docker stop -t <seconds>`.

## Archives

- The entire database directory, scripts included, can be zipped into an archive stored in a separate `ArchiveStoragePath`, named after its creation time in UTC (`zestdb_archive_2026-11-15_10-00-00.123456789.zip`).
- Automatic archiving at a configurable `ArchiveCreationDelay` (in seconds), resuming from the most recent archive on restart. It can be disabled entirely with `AutoArchiveSaving`.
- Manual archiving on demand via the `save` command (admin role).
- Commands are blocked only while the shards are flushed and the file sizes recorded. Compaction is paused while the flushed files are copied to a snapshot, and the zip is built from that copy.
- `ArchiveRetention` keeps the N most recent archives and deletes the older ones.
- `lsaves` (admin role) lists the available archives, oldest first.
- `restore <archive.zip>` (admin role) replaces the database with an archive of `ArchiveStoragePath`.
  - The archive is extracted first while the database keeps serving; commands are then blocked only while the shards are closed, swapped and reopened.
  - If anything fails, the previous data is kept.
  - The archive's WAL is not replayed (archives are taken right after a flush, so it is empty).
  - Only the data is restored: the scripts currently loaded are kept.

## Database summary

`info` (or `stats`, read role) returns the number of keys, the size of the database on disk, the cache hit ratio and the most recent archive as JSON (see [commands](commands.md)).

## HTTP monitoring

- A separate HTTP server (`WebPort`, default `1237`) exposes live per-shard performance statistics under `/`, as JSON: `{"stats": {"shard0": {...}, "shard1": {...}, ...}}`. It uses TLS when `useSSL` is set and the `NetworkValidation` allow-list.
- Statistics include request counts, cache misses, average latency and cache-miss percentage for each operation type (`get`, `set`, `del`, `getby`, `setby`, `delby`, `exists`, `count`, `keys`) plus an aggregate total.
  - `exists`, `count` and `keys` only read the in-memory index, so they never record a cache miss.
  - `count` and `keys` are counted once per shard, `exists` on the key's shard only.
- The counters are per-shard, lock-free atomics, and can be used to observe hot spots or cache efficiency.

## Prometheus metrics

The same HTTP server serves every statistic at `/metrics`, in the Prometheus text format (same TLS and allow-list as `/`). Grafana reads them through Prometheus, added as a data source.

| Metric | Type | Labels | Meaning |
| --- | --- | --- | --- |
| `zestdb_requests_total` | counter | `shard`, `op` | Requests handled (`nbRequests` of `/`). |
| `zestdb_cache_misses_total` | counter | `shard`, `op` | Requests whose value was read from disk (`nbCacheMisses`). |
| `zestdb_request_duration_seconds` | histogram | `shard`, `op`, `le` | Request latency, with buckets from 0.1 ms to 1 s. |
| `zestdb_request_latency_avg_seconds` | gauge | `shard`, `op` | Mean latency since startup (`avgLatencyMs`, in seconds). |
| `zestdb_cache_miss_ratio` | gauge | `shard`, `op` | Share of cache misses since startup (`avgCacheMissPercent`, from 0 to 1). |
| `zestdb_keys` | gauge | `shard` | Keys stored; expired keys count until the next compaction. |
| `zestdb_live_bytes` | gauge | `shard` | Bytes of segment data the live keys point to. |
| `zestdb_stored_bytes` | gauge | `shard` | Bytes written to the segments, live or dead. |
| `zestdb_cache_entries` | gauge | `shard` | Keys in the LRU cache. |
| `zestdb_cache_capacity` | gauge | `shard` | Capacity of the LRU cache (`CacheSize`). |
| `zestdb_compactions_total` | counter | `shard` | Compactions run since startup. |
| `zestdb_rate_limited_total` | counter | `user` | Commands refused by the user's rate or bandwidth limit (users with a limit only). |
| `zestdb_connections` | gauge | | Open TCP client connections. |

`op` is one of `get`, `set`, `del`, `getby`, `setby`, `delby`, `exists`, `count`, `keys`. The `total` of `/` has no series of its own: sum over the labels instead. The counters restart from zero with the server, which `rate()` handles.

Scrape configuration:
```yaml
scrape_configs:
  - job_name: zestdb
    static_configs:
      - targets: ["zestdb-host:1237"]
    # scheme: https   # with useSSL
```

Some queries:
```promql
sum by (op) (rate(zestdb_requests_total[1m]))                                            # operations per second
histogram_quantile(0.99, sum by (le, op) (rate(zestdb_request_duration_seconds_bucket[5m])))  # p99 latency
sum(rate(zestdb_cache_misses_total[5m])) / sum(rate(zestdb_requests_total[5m]))          # cache miss ratio
1 - sum(zestdb_live_bytes) / sum(zestdb_stored_bytes)                                    # share of dead data
```

## Logging

- Multi-level colored logging (`INFO`, `DEBUG`, `WARNING`, `ERROR`, `CRITICAL`) with timestamps. Control characters in messages are escaped.
- Debug level can be enabled/disabled at runtime through the configuration (`isDebug`).
- Scripts write to the same log with `print` and `zest.log` (see [scripting](scripting.md#api)).
