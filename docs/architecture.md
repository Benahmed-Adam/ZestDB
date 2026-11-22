# Architecture

ZestDB follows a layered design where each layer is independently testable:

```
                    +---------------------+
   TCP/SSL Server   |  Server / Session   |
                    +---------------------+
                            |
                    +---------------------+
       Command layer        |  ZestDB (execCmd)  --  ScriptEngine (Lua)
                    +---------------------+
                            |
                    +---------------------+
                    |    ShardManager     |   routing + parallel bulk
                    +---------------------+
          +---------------+-----+---------------+
          | Shard 0       | ... | Shard N-1      |
          +---------------+-----+---------------+
          | IndexManager  |  cache-free reads   |
          | StorageManager|  append-only segs   |
          | LRUCache      |  in-memory hot data |
          | WAL           |  durability log     |
          | PerfMonitoring|  metrics            |
          +---------------+-----+---------------+
```

- `ZestDB` is the main facade: it parses commands, applies validation rules, and coordinates shards. It also owns the HTTP server and the background flush/archive threads.
- `ScriptEngine` holds the Lua scripts, their functions and triggers, with one Lua state per server thread (see [scripting](scripting.md)).
- `ShardManager` routes each key to a shard by hash and parallelizes bulk operations over the thread pool.
- Each `Shard` owns a self-contained storage engine (index, segments, cache, WAL) and a background compactor thread.

## Threading model

- Main thread: runs the HTTP stats server, which has its own worker threads.
- Console thread: reads the commands typed in the interactive REPL.
- Asio IO threads (one `io_context` per CPU core, the first one also accepts connections): each TCP/SSL session stays on one thread, which parses and executes its commands, scripts included.
- Per-shard compactor threads: index and segment compaction.
- Global flush thread: periodic disk synchronization.
- Global archive (save) thread: periodic database zipping.
- Thread pool: parallel execution of bulk `getBy`/`setBy`/`delBy` operations.
- Shared-mutex (`shared_mutex`) per shard allows concurrent reads while serializing writes.

## Sharding

- Data is partitioned over 32 shards using `std::hash` on the key. The number is fixed at compile time (`NUM_SHARDS` in `src/ZestDB.hpp`): changing it makes an existing database unreadable, since keys would map to other shards.
- Each shard owns its own index, storage segments, LRU cache, WAL and performance counters, allowing independent background maintenance.
- Parallel bulk operations are dispatched through an internal thread pool sized to the number of hardware threads.

## Log-structured storage

- Values are appended to fixed-size, append-only data segments (`*.seg`).
- A value is stored zstd-compressed (level 3, with a checksum) when that makes it smaller; short or incompressible values are stored as is, and values written by older versions stay readable. The LRU cache keeps decompressed values.
- When a segment reaches the configured capacity (`SegSize`), a new segment is automatically created (segment rotation).
- Deletes are implemented as tombstones rather than physical removal, keeping the append-only model.
- Unused (fully obsolete) segments are physically removed during compaction.

## In-memory index

- An index file maps every key to its location (segment id, offset, size) on disk.
- Every live entry (segment, offset, size) is kept in an in-memory `unordered_map`: lookups and pattern scans never read the index file.
- The index file is an append-only log: a set appends the new entry, a delete appends a tombstone, and loading replays the log in order (the last record for a key wins). Writes are sequential and buffered, with no seek.
- Periodic compaction rewrites the index file, dropping tombstones, expired keys and unreachable entries.

## LRU cache

- A per-shard LRU (Least Recently Used) cache stores recently read key-value pairs in memory. Writes only refresh keys already cached, so they do not evict the hot keys.
- On startup, the cache is pre-filled (warm-up) with up to `CacheSize` entries of the index.
- Cache capacity per shard is configurable via `CacheSize`.
- The cache reports cache-miss ratio per operation type for monitoring.

## Write-ahead log (WAL)

- Every successful write is appended to a WAL file, which guarantees durability across unexpected shutdowns.
- Writes are journaled as their effect, not as the command: `setex` / `expire` as an absolute date (`setexat` / `expireat`), and `incr`, `setnx`, `cas` and values changed by a script trigger as the value they wrote. Replaying the WAL therefore gives the same state, without running any trigger again.
- Optional `fsyncOnWrite` to force a synchronous fsync after every write.
- On startup, WAL entries are collected, sorted by sequence number, and replayed to rebuild the in-memory state, after which the WAL is cleared.
- The WAL is flushed and cleared together with the index and segments during a flush cycle.

## Durability and background maintenance

- A flush thread periodically syncs in-memory changes (index and segments) to disk at a configurable `FlushInterval`.
- A compactor thread per shard checks every second how much of its segment data is dead (overwritten or deleted values). It compacts the index and the segments once half of the data is dead, or after `CompactingInterval` seconds at most, which also drops the expired keys. Only one shard compacts at a time, so the shards are never all blocked together.
- A verification pass on boot detects and removes index entries that no longer point to valid on-disk data.

## Data storage model

The database directory (`DbPath`) is organized as follows:

```
<DbPath>/
  scripts/
    <name>.lua     # Lua scripts, loaded at startup
  shards/
    <shard_id>/
      INDEX        # binary index of key -> (segment, offset, size)
      WAL          # write-ahead log of mutating commands
      seg/
        <id>.seg   # append-only value segments
```

### On-disk formats

- **Index**: an append-only log of records, each a packed header (`klen`, `offset`, `segmentId`, `size`, `isTombstone`, `expiresAt`) followed by the key bytes; the last record of a key wins.
- **Segments**: raw concatenated values, referenced by offset and size from the index.
- **WAL**: length-prefixed commands followed by a sequence number and a newline, sorted by sequence number on replay.

### Lifecycle

1. A `set` appends the value to the current segment, appends an index record, refreshes the cache if the key is in it and appends a WAL record.
2. A repeated `set` on the same key appends a new index record, which replaces the previous one; nothing is rewritten in place.
3. A `del` appends a tombstone record to the index and evicts the key from the cache.
4. Compaction rewrites the index (without tombstones and expired keys), moves the live values out of mostly dead segments and deletes the segments nothing points to.
5. Flushing fsyncs the index and segments to disk and clears the WAL.
