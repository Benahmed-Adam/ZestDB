# ZestDB

ZestDB is a sharded key-value store written in C++20. Each of its 32 shards has its own write-ahead log, append-only data segments, in-memory index and LRU cache, and the server answers over a length-prefixed TCP protocol (optionally TLS).

- `get` / `set` / `del`, plus `setex`, `expire`, `ttl` and `persist` for keys that expire
- Atomic `incr` / `decr`, `setnx` and `cas`
- Pattern commands: `getby`, `setby`, `delby`, `count` and `keys` by prefix, suffix, substring or regex, with `lim`
- Lua scripting: functions called in one round trip, and triggers that validate, transform or react to writes, in a sandbox
- Users with `read`, `write` or `admin` roles, auth lockout, idle and auth timeouts
- Zip archives with retention, and `restore`
- HTTP stats server

Source and full documentation: https://github.com/Benahmed-Adam/ZestDB

## Quick start

```bash
docker run -dit --name zestdb \
  -p 7321:7321 -p 1237:1237 \
  -v zestdb-data:/data/db \
  -v zestdb-archive:/data/archive \
  -v "$(pwd)/config.yaml:/data/config.yaml" \
  benahmedadam/zestdb:latest
```

| Port | Use |
| --- | --- |
| `7321` | Database (TCP, length-prefixed frames) |
| `1237` | HTTP stats (JSON) |

| Path | Content |
| --- | --- |
| `/data/db` | Database files and Lua scripts (volume) |
| `/data/archive` | Zip archives (volume) |
| `/data/config.yaml` | Configuration |

- **Use named volumes** (`-v name:/data/db`): without them Docker creates anonymous volumes that a new `docker run` does not reuse.
- **The container runs a REPL**:
  - `docker attach zestdb` gives you a `zestdb>` prompt with admin rights, and `Ctrl+P` `Ctrl+Q` detaches.
  - This needs `-it` (here `-dit`); without it the server runs but nothing reads the terminal.
- **The server runs as user `zest` (uid 10001)**: files you mount must be readable by it.

```
zestdb> s name "Bob"
{"affectedRows":1,"code":0,"response":""}
zestdb> g name
{"affectedRows":1,"code":0,"response":"Bob"}
```

## Configuration and users

The image ships **with no user account**, so no TCP client can authenticate until you mount your own `config.yaml` over `/data/config.yaml`. Start from [the image's default one](https://github.com/Benahmed-Adam/ZestDB/blob/main/docker/config.yaml) and add your users:

```yaml
users:
  - user: alice
    role: admin   # read, write or admin
    tokenHash: <sha256 of sha256("alice" + password)>
```

Compute the hash with:

```bash
printf %s "$(printf %s "<user><password>" | sha256sum | cut -d' ' -f1)" | sha256sum | cut -d' ' -f1
```

Clients then authenticate with `Authorization: alice.<sha256("alice" + password)>`. See [the protocol](https://github.com/Benahmed-Adam/ZestDB/blob/main/docs/protocol.md), and the [reference Python client](https://github.com/Benahmed-Adam/ZestDB/blob/main/benchmark/zest_client.py).

The image's default configuration differs from the repository's:

| Setting | Value | Why |
| --- | --- | --- |
| `NetworkValidation` | `'.*'` | In the container every client comes from the Docker network. Limit access with the ports you publish, or set `useSSL: true` with your certificate and key before exposing the server. |
| `fsyncOnWrite` | `false` | Faster writes. Set it to `true` if every write must reach the disk before it is acknowledged. |
| `AutoArchiveSaving` | `false` | No automatic archive. Use `save`, or set it to `true` for an archive every `ArchiveCreationDelay` seconds. |

All the settings are described in [the configuration page](https://github.com/Benahmed-Adam/ZestDB/blob/main/docs/configuration.md).

## Lua scripts

Scripts live in `/data/db/scripts/<name>.lua` and are loaded at startup. Two ways to add one:

- **Send it** with `script load <name> "<code>"` from an admin client or the REPL. It is written to the volume, so it survives restarts.
- **Mount a folder of scripts**, kept with your code, then run `script reload` to load them:
  ```bash
  -v "$(pwd)/scripts:/data/db/scripts:ro"
  ```
  The folder is read-only, so `script load` and `script del` then fail with code `502`.

See [the scripting guide](https://github.com/Benahmed-Adam/ZestDB/blob/main/docs/scripting.md) and [ready-to-use examples](https://github.com/Benahmed-Adam/ZestDB/tree/main/scripts_examples).

## Archives

`save` writes a zip of `/data/db` to `/data/archive`, and `lsaves` lists them. `restore <archive.zip>` replaces the data with one of them; the scripts loaded at that moment are kept. See [operations](https://github.com/Benahmed-Adam/ZestDB/blob/main/docs/operations.md#archives).

## Stopping

`docker stop zestdb` sends `SIGTERM`. ZestDB stops accepting connections, lets each session finish its current request and send the reply (up to 5 seconds), then flushes every shard before exiting. Docker kills the container after 10 seconds, which leaves time for both steps.

## Tags

- `latest`: the last commit of `main`.
- `sha-<commit>`: a given commit (7 characters).

Both are built once the tests pass on Linux and Windows.
