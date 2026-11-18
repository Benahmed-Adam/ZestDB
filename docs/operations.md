# Operations

## Running the server

```bash
make run            # or ./bin/zestdb
./bin/zestdb pop    # populate a million keys on startup (demo)
```

The process also starts the HTTP monitoring server and an interactive REPL. Type `q` / `quit` to exit gracefully.

## Docker

An image is published on Docker Hub as `benahmedadam/zestdb` (`latest`, and `sha-<commit>` for each commit on `main`). To build it locally: `docker build -t zestdb .` The Docker Hub page is [`docker/README.md`](../docker/README.md): update it with the image.
```bash
docker run -it -p 7321:7321 -p 1237:1237 \
  -v zestdb-data:/data/db -v zestdb-archive:/data/archive \
  -v "$(pwd)/config.yaml:/data/config.yaml" \
  benahmedadam/zestdb:latest
```
- `-it` connects the interactive REPL; without it the server still runs, but nothing reads the commands typed in the terminal. Use `-dit` and `docker attach` to run it in the background and reach the REPL later (`Ctrl+P` `Ctrl+Q` detaches).
- `/data/db` and `/data/archive` are volumes: name them as above, otherwise Docker creates anonymous volumes that a new `docker run` does not reuse.
- The default configuration (`docker/config.yaml`) accepts every address and has **no user account**, so no client can connect over TCP until you mount your own `config.yaml` over `/data/config.yaml` with users. The REPL works without one.
- `docker stop` sends `SIGTERM`, which flushes every shard before exiting.

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

## HTTP monitoring

- A separate HTTP server (`WebPort`, default `1237`) exposes live per-shard performance statistics under `/`, as JSON: `{"stats": {"shard0": {...}, "shard1": {...}, ...}}`. It uses TLS when `useSSL` is set and the `NetworkValidation` allow-list.
- Statistics include request counts, cache misses, average latency and cache-miss percentage for each operation type (`get`, `set`, `del`, `getby`, `setby`, `delby`, `exists`, `count`, `keys`) plus an aggregate total.
  - `exists`, `count` and `keys` only read the in-memory index, so they never record a cache miss.
  - `count` and `keys` are counted once per shard, `exists` on the key's shard only.
- The counters are per-shard, lock-free atomics, and can be used to observe hot spots or cache efficiency.

## Logging

- Multi-level colored logging (`INFO`, `DEBUG`, `WARNING`, `ERROR`, `CRITICAL`) with timestamps. Control characters in messages are escaped.
- Debug level can be enabled/disabled at runtime through the configuration (`isDebug`).
- Scripts write to the same log with `print` and `zest.log` (see [scripting](scripting.md#api)).
