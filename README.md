<p align="center">
  <img src="images/zestdb.png" alt="ZestDB Logo" width="350"/>
</p>

# ZestDB

ZestDB is a high-performance, disk-persistent key-value store written in modern C++ (C++20).

## Table of Contents

- [Features](#features)
- [Architecture](#architecture)
- [Commands](#commands)
- [Data Storage Model](#data-storage-model)
- [Configuration](#configuration)
- [Network Protocol](#network-protocol)
- [Build](#build)
- [Testing](#testing)
- [Tooling](#tooling)
- [Project Structure](#project-structure)
- [Benchmarks](#benchmarks)
---

## Features

### Core Key-Value Operations
- `set`, `get`, `del` operations on string keys and values, with configurable size limits.
- Case-insensitive command parsing with short aliases (`s`, `g`, `d`).
- Validation of key length (`MaxKeySize`) and value length (`MaxValueSize`).
- Optional `jsonOnly` mode that rejects any value that is not valid JSON.
- `readOnly` mode that blocks all write operations while still allowing reads.

### Bulk / Pattern Operations
- `gb (getBy)`, `sb (setBy)`, ` db (delBy)` commands to query or modify keys that match a user-provided validation rule.
- Four matching modes:
  - `re` - regular expression (ECMAScript syntax via `std::regex`).
  - `sw` - key starts with a given prefix.
  - `ct` - key contains a given substring.
  - `ew` - key ends with a given suffix.
- Optional `lim <n>` parameter to cap the number of affected entries, enforced globally across all shards.
- Bulk operations are executed in parallel across shards and results are merged.

### Sharding
- Data is partitioned over a configurable number of shards (default 32) using `std::hash` on the key.
- Each shard owns its own index, storage segments, LRU cache, WAL and performance counters, allowing independent background maintenance.
- Parallel bulk operations dispatched through an internal thread pool sized to the number of hardware threads.

### Log-Structured Storage
- Values are appended to fixed-size, append-only data segments (`*.seg`).
- When a segment reaches the configured capacity (`SegSize`), a new segment is automatically created (segment rotation).
- Deletes are implemented as tombstones rather than physical removal, keeping the append-only model.
- Unused (fully obsolete) segments are physically removed during compaction.

### In-Memory Index
- An index file maps every key to its location (segment id, offset, size) on disk.
- Every live entry (segment, offset, size) is kept in an in-memory `unordered_map`: lookups and pattern scans never read the index file.
- The index file is an append-only log: a set appends the new entry, a delete appends a tombstone, and loading replays the log in order (the last record for a key wins). Writes are sequential and buffered, with no seek.
- Periodic compaction rewrites the index file, dropping all tombstones and unreachable entries.

### LRU Caching
- A per-shard LRU (Least Recently Used) cache stores recently accessed key-value pairs in memory.
- On startup, the cache is pre-filled (warm-up) with the most recently stored entries in the index.
- Cache capacity per shard is configurable via `CacheSize`.
- The cache reports cache-miss ratio per operation type for monitoring.

### Write-Ahead Log (WAL)
- Every successful mutating command is appended to a WAL file, which guarantees durability across unexpected shutdowns.
- Optional `fsyncOnWrite` to force a synchronous fsync after every write.
- On startup, WAL entries are collected, sorted by timestamp, and replayed to rebuild the in-memory state, after which the WAL is cleared.
- The WAL is flushed and cleared together with the index and segments during a flush cycle.

### Durability and Background Maintenance
- A flush thread periodically syncs in-memory changes (index and segments) to disk at a configurable `FlushInterval`.
- A compactor thread per shard runs at a configurable `CompactingInterval` to compact the index and delete unused data segments.
- A verification pass on boot detects and removes index entries that no longer point to valid on-disk data.

### Network Server
- Custom asynchronous TCP server built on Asio, with one `io_context` per CPU core: each session stays on one thread, so requests are never handed between threads. Batched commands are acknowledged after a single WAL write (group commit).
- Batch framing: a single request can carry multiple commands packed with 4-byte length prefixes, with one JSON response per command.
- Per-request JSON output with `code`, `response` and `affectedRows` fields.
- Authentication via a `Authorization: <user>.<sha256(user+password)>` handshake command before any data operation, with per-user roles (`read`, `write`, `admin`) checked on every command, so a `reload` that removes a user or changes its role applies to open sessions.
- A user is locked out for 60 seconds after 5 failed authentications. The lock is per user rather than per address, so it still works behind a proxy such as ngrok (where every client comes from `127.0.0.1`); the flip side is that anyone can lock a known user out for a minute.
- Connection limits: `MaxConnections` simultaneous sessions, `AuthTimeout` to finish the TLS handshake and authenticate, `IdleTimeout` for authenticated sessions.
- Configurable IP allow-listing using a regular expression (`NetworkValidation`), applied to both the TCP server and the HTTP stats server.
- Optional TLS/SSL support (both the TCP server and the HTTP server can use certificates).

### HTTP Monitoring Server
- A separate HTTP server (`WebPort`, default `1237`) exposes live per-shard performance statistics under `/`.
- Statistics include request counts, cache misses, average latency and cache-miss percentage for each operation type (`get`, `set`, `del`, `getby`, `setby`, `delby`) plus an aggregate total.

### Performance Monitoring
- Per-shard, lock-free atomic counters track request counts, cache misses and latency for every operation type.
- Aggregated statistics are exposed through the HTTP server and can be used to observe hot spots or cache efficiency.

### Configuration Management
- All settings are read from a `config.yaml` file with sensible defaults when fields are missing.
- Hot-reload of the configuration via the `reload` / `r` command, with immutability guards on path and port settings.
- Runtime modification of most parameters through the `scfg` command without restarting the server.
- `gcfg` returns the full active configuration as JSON.
- Changes made via `scfg` are persisted back to `config.yaml`.

### Archiving
- The entire database directory can be zipped into a versioned archive stored in a separate `ArchiveStoragePath`.
- Automatic archiving at a configurable `ArchiveCreationDelay` (in seconds), resuming from the most recent archive on restart.
- Manual archiving on demand via the `save` command (admin role).
- Commands are blocked only while the database files are copied to a snapshot; the zip is built from the copy afterwards.
- `ArchiveRetention` keeps the N most recent archives and deletes the older ones.
- Can be disabled entirely with `AutoArchiveSaving`.

### Logging
- Multi-level colored logging (`INFO`, `DEBUG`, `WARNING`, `ERROR`, `CRITICAL`) with timestamps.
- Debug level can be enabled/disabled at runtime through the configuration (`isDebug`).

---

## Architecture

ZestDB follows a layered design where each layer is independently testable:

```
                    +---------------------+
   TCP/SSL Server   |  Server / Session   |
                    +---------------------+
                            |
                    +---------------------+
       Command layer        |  ZestDB (execCmd)
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
- `ShardManager` routes each key to a shard by hash and parallelizes bulk operations over the thread pool.
- Each `Shard` owns a self-contained storage engine (index, segments, cache, WAL) and a background compactor thread.

### Threading Model
- Main thread: command parsing and dispatch.
- Asio IO threads (one `io_context` per CPU core, the first one also accepts connections): each TCP/SSL session stays on one thread. The HTTP stats server has its own threads.
- Per-shard compactor threads: index and segment compaction.
- Global flush thread: periodic disk synchronization.
- Global archive (save) thread: periodic database zipping.
- Thread pool: parallel execution of bulk `getBy`/`setBy`/`delBy` operations.
- Shared-mutex (`shared_mutex`) per shard allows concurrent reads while serializing writes.

---

## Commands

Commands are whitespace-delimited and case-insensitive. For multi-word values, everything after the key/pattern is preserved verbatim.

| Command | Description |
| --- | --- |
| `get <key>` / `g <key>` | Retrieve the value stored for a key. |
| `set <key> <value>` / `s <key> <value>` | Store a value (spaces preserved). |
| `del <key>` / `d <key>` | Delete a key (marks a tombstone). |
| `getby <mode> <pattern> [lim <n>]` / `gb ...` | Get all entries whose key matches the rule. |
| `setby <mode> <pattern> <value> [lim <n>]` / `sb ...` | Update the value of all matching keys. |
| `delby <mode> <pattern> [lim <n>]` / `db ...` | Delete all matching keys. |
| `flush` / `f` | Force all in-memory data to disk and clear the WAL. |
| `reload` / `r` | Reload the configuration from `config.yaml`. |
| `scfg <param> <value>` | Set a configuration parameter at runtime. |
| `gcfg` | Print the full active configuration as JSON. |
| `save` | Manually create a zip archive of the database. |
| `help` / `h` | Print command and usage help. |

### Pattern Modes
- `re` - regex match on the full key.
- `sw` - key starts with the pattern.
- `ct` - key contains the pattern.
- `ew` - key ends with the pattern.
- `lim <n>` - global cap on the number of matched/affected entries.

### Example Session

```
zestdb> s name Bob
{"code":0,"response":"Key successfully set","affectedRows":1}
zestdb> g name
{"code":0,"response":"Bob","affectedRows":1}
zestdb> gb sw na
{"code":0,"response":[{"name":"Bob"}],"affectedRows":1}
zestdb> scfg useSSL true
{"code":0,"response":"Values successfuly updated !","affectedRows":0}
zestdb> d name
{"code":0,"response":"Key successfully deleted","affectedRows":1}
```

### `scfg` Parameters

| Parameter | Type | Notes |
| --- | --- | --- |
| `SegSize` | int | Must be greater than `MaxValueSize`. |
| `MaxKeySize` | int | Cannot exceed the internal limit (256). |
| `MaxValueSize` | int | Must be less than `SegSize`. |
| `CacheSize` | int | Per-shard cache capacity. |
| `CompactingInterval` | int | Seconds between compactions. |
| `FlushInterval` | int | Seconds between disk syncs. |
| `NetworkValidationStr` | string | IP allow-list regex. |
| `isDebug` | bool | Enable debug logging (`true/false` or `1/0`). |
| `jsonOnly` | bool | Reject non-JSON values. |
| `readOnly` | bool | Block write operations. |
| `ArchiveCreationDelay` | int | Seconds between automatic archives. |
| `AutoArchiveSaving` | bool | Enable/disable automatic archiving. |

`scfg`, `reload`, `gcfg`, `save` and `flush` require the `admin` role. `useSSL`, the ports and the connection limits can only be changed in `config.yaml`, followed by a restart.

---

## Data Storage Model

The database directory (`DbPath`) is organized as follows:

```
<DbPath>/
  shards/
    <shard_id>/
      INDEX        # binary index of key -> (segment, offset, size)
      WAL          # write-ahead log of mutating commands
      seg/
        <id>.seg   # append-only value segments
```

### On-disk formats
- **Index**: records serialized as a packed header (`klen`, `offset`, `segmentId`, `size`, `isTombstone`) followed by the key bytes.
- **Segments**: raw concatenated values, referenced by offset and size from the index.
- **WAL**: length-prefixed commands with a millisecond timestamp and a trailing newline, sorted by timestamp on replay.

### Lifecycle
1. A `set` appends the value to the current segment, records an index entry, updates the cache and appends a WAL record.
2. A repeated `set` on the same key tombstones the old index entry and appends the new one.
3. A `del` updates the index entry to a tombstone and evicts the key from the cache.
4. Compaction rewrites the index (removing tombstones) and physically deletes obsolete segments.
5. Flushing fsyncs the index and segments to disk and clears the WAL.

---

## Configuration

Configuration is provided through `config.yaml` in the working directory. All fields are optional and fall back to defaults.

| Key | Default | Description |
| --- | --- | --- |
| `DbPath` | current dir | Folder containing the database files. |
| `SegSize` | 128000 | Maximum size of a data segment (bytes). |
| `MaxKeySize` | 64 | Maximum key length. |
| `MaxValueSize` | 10000 | Maximum value length. |
| `CacheSize` | 1000 | LRU cache capacity per shard. |
| `CompactingInterval` | 3600 | Seconds between compaction runs. |
| `FlushInterval` | 120 | Seconds between disk syncs. |
| `DBPort` | 7321 | TCP data server port. |
| `WebPort` | 1237 | HTTP monitoring server port. |
| `NetworkValidation` | ".*" | Regex for allowed client IP addresses, TCP and HTTP stats (empty also allows all). The provided `config.yaml` sets `^127\.0\.0\.1$` (local only). |
| `isDebug` | false | Enable debug logging. |
| `jsonOnly` | false | Accept only JSON values. |
| `readOnly` | false | Block write operations. |
| `fsyncOnWrite` | true | Force fsync on every write. |
| `useSSL` | false | Enable TLS on the servers. |
| `SSLCertPath` | "" | Path to the TLS certificate (PEM). |
| `SSLKeyPath` | "" | Path to the TLS private key (PEM). |
| `ArchiveStoragePath` | ./archive/ | Folder where zip archives are written. |
| `ArchiveCreationDelay` | 3600 | Seconds between automatic archives. |
| `AutoArchiveSaving` | true | Enable automatic archiving. |
| `ArchiveRetention` | 10 | Number of archives kept, the oldest are deleted (`0` keeps all). |
| `MaxConnections` | 1024 | Maximum number of simultaneous client connections. |
| `AuthTimeout` | 10 | Seconds a client has to finish the TLS handshake and authenticate. |
| `IdleTimeout` | 300 | Seconds of inactivity before an authenticated connection is closed. |
| `users` | empty | List of `{user, role, tokenHash}` entries. `role` is `read` (`get`, `getby`), `write` (+ `set`, `del`, `setby`, `delby`, the default) or `admin` (+ `flush`, `reload`, `scfg`, `gcfg`, `save`). `tokenHash` is `sha256(token)`, where the token is `sha256(user+password)` (see below). A plaintext `password` field is still accepted but deprecated. |

The provided `config.yaml` contains a development account (`bob`/`bob`, admin) and only accepts local connections: replace the account before widening `NetworkValidation`.

---

## Network Protocol

Clients communicate with the TCP server using a length-prefixed, big-endian framing.

### Frame format
```
4 bytes      payload length (network byte order)
N bytes      concatenated commands
```
Each command within the payload is itself length-prefixed:
```
4 bytes      command length (network byte order)
M bytes      command string
```

### Authentication
A session must first send:
```
Authorization: <username>.<sha256(username+password)>
```
The server replies with `{"code":0,...}` on success, or closes the connection on failure. Commands sent before authentication are rejected.

`config.yaml` only stores a hash of the token, so a leaked config does not grant access. To compute the `tokenHash` for a user:
```bash
printf %s "$(printf %s "<user><password>" | sha256sum | cut -d' ' -f1)" | sha256sum | cut -d' ' -f1
```

Without `useSSL` the token travels in clear and a captured token can be replayed, so enable SSL outside a trusted network.

Before authentication a request payload is limited to 1 MiB, and to 64 MiB afterwards; a larger size closes the connection.

### Response format
Each command yields one JSON object:
```json
{"code":0,"response":"Bob","affectedRows":1}
```
- `code`: `0` on success, `1` on error.
- `response`: a string, or a JSON array/object for bulk operations.
- `affectedRows`: number of affected entries.

Multiple commands in one request are answered with responses separated by `\r\n\r\n` and terminated by a trailing newline. See `benchmark/zest_client.py` for a minimal reference client.

---

## Build

### Requirements
- A C++20 compiler: GCC or Clang on Linux/macOS, MSVC (Visual Studio 2022) or MinGW on Windows.
- CMake 3.25+, `make` for the Makefile shortcuts (Linux/macOS), and Perl 5 when OpenSSL has to be built (plus NASM with MSVC).

On Windows, use CMake directly (the Makefile relies on Unix tools), from a PowerShell where the **x64** MSVC environment is loaded (the default "Developer PowerShell" is x86):
```powershell
& "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\Tools\Launch-VsDevShell.ps1" -Arch amd64 -HostArch amd64 -SkipAutomaticLocation
cmake -S . -B build\msvc -G Ninja -DCMAKE_BUILD_TYPE=Release -DOPENSSL_ROOT_DIR="C:/Program Files/OpenSSL-Win64"
cmake --build build\msvc --parallel
$env:PATH = "C:\Program Files\OpenSSL-Win64\bin;$env:PATH"   # the OpenSSL DLLs, needed to run zestdb.exe and the tests
ctest --test-dir build\msvc --output-on-failure
```
OpenSSL 3 and 4 both work.
Building OpenSSL from source there needs Strawberry Perl and NASM; installing OpenSSL 3 beforehand (and setting `OPENSSL_ROOT_DIR` if CMake does not find it) avoids that step.

No library has to be installed: on the first build, CMake (`FetchContent`) downloads and builds pinned versions of the dependencies. OpenSSL is the exception: an installed OpenSSL 3 (e.g. `libssl-dev`) is used when found, otherwise it is built from source.

| Library | Version | Used for |
| --- | --- | --- |
| OpenSSL | system 3.x, else 3.5.9 (via openssl-cmake 4.1.0) | TLS, SHA-256 |
| Asio (standalone) | 1.38.2 | TCP/SSL server |
| libzip + zlib | 1.12.1 + 1.3.2 | archives |
| libzippp | 7.1-1.10.1 | archives (C++ wrapper) |
| Catch2 | 3.16.1 | test suite |

The vendored headers in `src/lib/` (httplib, nlohmann/json, fkyaml) are part of the repository. The first build takes about a minute, or two when OpenSSL is built from source; the dependencies are kept in `build/<Release|Debug>/_deps` and reused by later builds (`make clean` removes them).

### Compile
```bash
make
```
The resulting binary is placed in `bin/zestdb`. `make` is a thin wrapper around CMake; the equivalent direct commands are:
```bash
cmake -S . -B build/Release -DCMAKE_BUILD_TYPE=Release
cmake --build build/Release --parallel
```

### Debug build
```bash
make DEBUG=1
```

### Run
```bash
make run            # or ./bin/zestdb
./bin/zestdb pop    # populate a million keys on startup (demo)
```

The process also starts the HTTP monitoring server and an interactive REPL. Type `q` / `quit` to exit gracefully.

---

## Testing

The project ships a test suite built with Catch2, covering every subsystem:

| Area | File |
| --- | --- |
| Data segments | `tests/test_DataSegment.cpp` |
| Index manager | `tests/test_IndexManager.cpp` |
| Storage manager | `tests/test_StorageManager.cpp` |
| Shard | `tests/test_Shard.cpp` |
| Shard manager | `tests/test_ShardManager.cpp` |
| WAL | `tests/test_WAL.cpp` |
| LRU cache | `tests/test_LRUCache.cpp` |
| Thread pool | `tests/test_ThreadPool.cpp` |
| Performance monitoring | `tests/test_PerfMonitoring.cpp` |
| Logger | `tests/test_Logger.cpp` |
| Server protocol | `tests/test_Server_Protocol.cpp` |
| Server sessions (auth, payload limit) | `tests/test_Server_Session.cpp` |
| ZestDB commands | `tests/test_ZestDB_Commands.cpp` |
| ZestDB utilities | `tests/test_ZestDB_Utils.cpp` |
| ZestDB validation | `tests/test_ZestDB_Validation.cpp` |

### Run tests
```bash
make tests
```
All test files are compiled into a single executable, `build/Release/zestdb_tests` (`build/Debug/` with `DEBUG=1`), and `make tests` runs each test case through `ctest`. To run a subset directly, use Catch2 tags or names:
```bash
./build/Release/zestdb_tests "[shard]"
```

### Code formatting
```bash
make format
```
Uses `clang-format` with the provided `.clang-format` configuration.

### Continous integration
A GitHub Actions workflow (`c-cpp.yml`) builds the project and runs the full test suite on every push and pull request to `main`.

---

## Project Structure

```
├── CMakeLists.txt                # build definition, fetches the dependencies
├── Makefile                      # build / test / run / format targets (wraps CMake)
├── config.yaml                   # runtime configuration
├── .github/workflows/            # CI pipeline
├── src/                          # C++ source
│   ├── ZestDB.{hpp,cpp}          # main facade, commands, config, archiving
│   ├── Server.{hpp,cpp}          # Asio TCP/SSL server and protocol
│   ├── Settings.hpp              # settings, results, messages, validation rules
│   ├── ShardManager.{hpp,cpp}    # shard routing and parallel bulk ops
│   ├── Shard.{hpp,cpp}           # per-shard storage engine + compactor
│   ├── IndexManager.{hpp,cpp}    # in-memory key index
│   ├── StorageManager.{hpp,cpp}  # segment lifecycle management
│   ├── DataSegment.{hpp,cpp}     # append-only value segments
│   ├── LRUCache.{hpp,cpp}        # per-shard LRU cache
│   ├── WAL.{hpp,cpp}             # write-ahead log
│   ├── PerfMonitoring.{hpp,cpp}  # statistics counters
│   ├── ThreadPool.{hpp,cpp}      # parallel task execution
│   ├── Logger.{hpp,cpp}          # multi-level colored logging
│   ├── File.hpp                  # fsync helper
│   ├── main.cpp                  # entry point, REPL, signal handling
│   └── lib/                      # vendored third-party headers
├── tests/                        # Catch2 unit tests
└── benchmark/                    # Python benchmark scripts
```

---

## Benchmarks

The `benchmark/` folder contains Python scripts (3.10+, standard library only) that measure a running server over the TCP protocol. Start the server first (`make run`), then run the scripts from the `benchmark/` folder:

| Script | Measures |
| --- | --- |
| `bench_kv.py` | `set` / `get` throughput and latency (avg, median, p95, p99) per batch and per command, for several volumes and batch sizes. |
| `bench_bulk.py` | Latency of `getby` / `setby` / `delby` for each mode (`sw`, `ct`, `ew`, `re`) and with `lim`, over a populated database. |
| `bench_concurrency.py` | Aggregate throughput with 1 to N clients in parallel (one process each), for a GET/SET mix. |
| `zest_client.py` | Shared client: framing, authentication, statistics. |

```bash
cd benchmark
python3 bench_kv.py --entries 100000,1000000 --batches 1,100,10000 --cleanup
python3 bench_bulk.py --entries 100000 --repeat 5
python3 bench_concurrency.py --clients 1,2,4,8,16,32,64,128 --ops 50000 --read-pct 80
```

Every script accepts `--host`, `--port`, `--user`, `--password` (default `bob` / `bob`) and `--json <file>` to save the results for comparison. The scripts write keys prefixed with `tk_` (`bench_kv.py`, removed with `--cleanup`), `bench_user:` / `bench_order:` and `bench_conc_` (removed at the end). Run them on a test database: the default `bench_kv.py` run writes 1 million keys.

### Results

Measured with the three commands above (`bench_concurrency.py` with `--clients 1,2,4,8,16,32,64,128`), client and server on the same machine (WSL2 Ubuntu, localhost, 12 cores), 32 shards, `CacheSize: 2000`, `FlushInterval: 10`, `fsyncOnWrite: false`, automatic archiving off. Numbers depend on the hardware: use them to compare runs, not as absolute figures.

#### SET / GET (`bench_kv.py`)

| Mode | Entries | Batch | Throughput (ops/s) | Avg per command (ms) | p99 per batch (ms) |
| --- | ---: | ---: | ---: | ---: | ---: |
| SET | 100,000 | 1 | 13,678 | 0.0705 | 0.262 |
| GET | 100,000 | 1 | 15,852 | 0.0608 | 0.196 |
| SET | 100,000 | 100 | 276,446 | 0.0034 | 1.070 |
| GET | 100,000 | 100 | 255,433 | 0.0037 | 1.010 |
| SET | 100,000 | 10,000 | 410,658 | 0.0023 | 26.321 |
| GET | 100,000 | 10,000 | 367,474 | 0.0026 | 31.188 |
| SET | 1,000,000 | 1 | 16,043 | 0.0601 | 0.197 |
| GET | 1,000,000 | 1 | 19,716 | 0.0491 | 0.157 |
| SET | 1,000,000 | 100 | 335,933 | 0.0028 | 0.708 |
| GET | 1,000,000 | 100 | 312,646 | 0.0030 | 0.660 |
| SET | 1,000,000 | 10,000 | 485,080 | 0.0019 | 56.925 |
| GET | 1,000,000 | 10,000 | 419,474 | 0.0023 | 34.023 |

- **Batching matters most**: one command per request is bound by the network round trip (~14-20k ops/s); batches of 100 give ~20x more, batches of 10,000 reach ~410-485k SET/s and ~370-420k GET/s.
- **Volume**: throughput holds from 100,000 to 1 million keys; the per-command latency stays in the microseconds.
- **No errors** across the 6.6 million operations.

#### Pattern commands (`bench_bulk.py`, 100,000 keys)

| Scenario | Command | Rows | Median (ms) |
| --- | --- | ---: | ---: |
| getby prefix, lim 100 | `gb sw bench_user: lim 100` | 100 | 0.65 |
| getby prefix, all | `gb sw bench_user:` | 50,000 | 21.02 |
| getby contains | `gb ct :12` | 1,111 | 4.24 |
| getby suffix | `gb ew 99` | 1,000 | 2.81 |
| getby regex | `gb re bench_user:\d*8` | 10,000 | 15.42 |
| getby regex, no match | `gb re (a+)+$` | 0 | 4.30 |
| setby prefix, lim 1000 | `sb sw bench_order: updated lim 1000` | 1,000 | 6.89 |
| setby prefix, all | `sb sw bench_order: updated` | 50,000 | 12.12 |
| delby prefix, lim 1000 | `db sw bench_order: lim 1000` | 1,000 | 7.29 |
| delby prefix, all | `db sw bench_` | 99,000 | 250.82 |

- Keys are matched in memory (the whole index is kept in RAM), so a scan of 100,000 keys with no match costs ~4 ms; the rest of the cost grows with the number of rows read or modified.
- The catastrophic-backtracking regex `(a+)+$` costs no more than a plain scan.

#### Concurrent clients (`bench_concurrency.py`, 80% GET / 20% SET, 50,000 ops per client, batch 100)

| Clients | Total ops | Throughput (ops/s) | Median batch (ms) | p99 batch (ms) |
| ---: | ---: | ---: | ---: | ---: |
| 1 | 50,000 | 392,334 | 0.209 | 0.792 |
| 2 | 100,000 | 465,032 | 0.390 | 0.762 |
| 4 | 200,000 | 476,870 | 0.783 | 1.576 |
| 8 | 400,000 | 507,256 | 1.502 | 2.539 |
| 16 | 800,000 | 565,354 | 2.047 | 5.357 |
| 32 | 1,600,000 | 544,194 | 5.064 | 9.299 |
| 64 | 3,200,000 | 511,576 | 10.957 | 20.485 |
| 128 | 6,400,000 | 492,270 | 25.017 | 39.593 |

- Each session runs on one of the server's IO threads (one `io_context` per core), so clients are served in parallel. Throughput holds at ~470-565k ops/s from 2 to 128 clients with no errors, while latency grows with the queue. The ceiling comes from the Python clients running on the same cores as the server: this measures the whole setup, not the server alone.
