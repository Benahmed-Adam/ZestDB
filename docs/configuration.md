# Configuration

Configuration is provided through `config.yaml` in the working directory. The file must exist, but all its fields are optional and fall back to the defaults below.

- `reload` / `r` reloads the file without restarting. The paths, ports, `useSSL` and the connection limits cannot change that way (code `402`): edit them and restart.
- `scfg <param> <value>` changes most parameters at runtime and writes them back to `config.yaml` (see [`scfg` parameters](commands.md#scfg-parameters)).
- `gcfg` returns the full active configuration as JSON.

## Settings

| Key | Default | Description |
| --- | --- | --- |
| `DbPath` | current dir | Folder containing the database files. |
| `SegSize` | 128000 | Maximum size of a data segment (bytes). |
| `MaxKeySize` | 64 | Maximum key length. |
| `MaxValueSize` | 10000 | Maximum value length. |
| `CacheSize` | 1000 | LRU cache capacity per shard. |
| `CompactingInterval` | 3600 | Maximum seconds between two compactions of a shard. A shard is compacted earlier, as soon as half of its stored data is dead (overwritten or deleted); the interval bounds how long expired keys stay on disk. |
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
| `rateLimit` | unlimited | Commands per second allowed to each user that sets no limit of its own (see [rate limits](#rate-limits)). |
| `burst` | `rateLimit` | Commands each such user may send at once. |
| `bandwidthLimit` | unlimited | Request bytes per second allowed to each such user. |
| `users` | empty | List of `{user, role, tokenHash}` entries (see below). |

The server refuses to start when `MaxValueSize` is not smaller than `SegSize`, when an interval, a timeout or `MaxConnections` is `0`, when `DBPort` equals `WebPort`, when `useSSL` is set without `SSLCertPath` and `SSLKeyPath`, when a role is unknown, or when a rate limit is not positive (`burst` must be at least 1). A `MaxKeySize` above 256 is lowered to 256.

The provided `config.yaml` contains a development account (`bob`/`bob`, admin) and only accepts local connections: replace the account before widening `NetworkValidation`.

## Users and roles

Each entry of `users` has a `user`, a `role` and a `tokenHash`:

| Role | Allowed commands |
| --- | --- |
| `read` | `get`, `getby`, `ttl`, `exists`, `count`, `keys`, `help` |
| `write` (the default) | `read` + `set`, `del`, `setby`, `delby`, `setex`, `setexat`, `expire`, `expireat`, `persist`, `incr`, `decr`, `setnx`, `cas`, `call` |
| `admin` | `write` + `flush`, `reload`, `scfg`, `gcfg`, `save`, `restore`, `lsaves`, `script` |

Roles are checked on every command, so a `reload` that removes a user or changes its role applies to open sessions.

`tokenHash` is `sha256(token)`, where the token is `sha256(user+password)` (the value the client sends, see [authentication](protocol.md#authentication)). `config.yaml` only stores a hash of the token, so a leaked config does not grant access. To compute it:
```bash
printf %s "$(printf %s "<user><password>" | sha256sum | cut -d' ' -f1)" | sha256sum | cut -d' ' -f1
```
A plaintext `password` field is still accepted but deprecated.

## Managing users at runtime

The `user` commands (admin role) change the users without a restart, and write them to `config.yaml`:

```
user add carol "s3cret" read      # role: read, write (default) or admin
user passwd carol "n3w-s3cret"
user role carol write
user del carol
user ls                           # [{"role":"write","user":"carol"}, ...]
```

- The password is sent in clear, as a quoted value, and only its hash is kept: enable `useSSL` outside a trusted network. These commands are not written to the WAL.
- A name has 1 to 64 printable characters, without `.` (it ends the name in the `Authorization` line), spaces, quotes or backslashes.
- `user del` and `user passwd` close the user's open connections: an idle one at once; a busy one finishes its current command, answers `302` (`SessionRevoked`) to the rest of its batch, then closes. `user role` closes nothing: the new role applies from the next command. A `reload` that removes a user or changes its `tokenHash` closes its connections the same way.
- The last admin cannot be deleted or lose the role (code `309`): the REPL stays admin, but remote administration would be lost.
- `config.yaml` is rewritten through a temporary file and a rename. Each entry keeps its other fields (its rate limits), a `password` field is replaced by the `tokenHash`, and new users are added at the end. Comments in the file are not kept.
- A new user has no limit of its own: the top-level rate limits apply.

## Rate limits

`rateLimit`, `burst` and `bandwidthLimit` can be set at the top of the file, for every user, and in a user's entry, for that user only:

```yaml
rateLimit: 500          # commands per second
burst: 1000             # commands at once (default: rateLimit)
bandwidthLimit: 5MB     # request bytes per second: a number, or a KB, MB or GB suffix (powers of 1024)

users:
  - user: bob
    role: write
    tokenHash: ...
    rateLimit: 1000     # bob's own limits
    burst: 2000
    bandwidthLimit: 10MB
  - user: alice         # no limit of her own: the top-level ones apply
    role: read
    tokenHash: ...
```

- Each limit a user does not set falls back to the top-level one, then to unlimited: without any of these fields, nobody is limited. A user who sets a `rateLimit` without a `burst` gets a burst equal to that `rateLimit`.
- The limits are per user, not shared by all users, and a user's connections share them: opening more connections does not raise them.
- Every command counts, so a batch of 10,000 commands counts 10,000; `call` counts as one command, whatever the script does.
- A user may send up to `burst` commands at once, then `rateLimit` per second (a token bucket). In a batch that exceeds it, the first commands run and the others are refused with code `304` (`RateLimited`), without being parsed.
- The bandwidth is checked before a request is read, from its length. A request larger than the limit still passes when the user has not used it recently, and the next requests wait until the average is back under the limit. A refused request is read and dropped without being stored, and each of its commands is answered `304`.
- `reload` applies new limits to open sessions without resetting what users have already used. The REPL is never limited, nor is the request that carries the `Authorization` line (it is capped at 1 MiB).
- `zestdb_rate_limited_total{user}` counts the refused commands of each limited user (see [Prometheus metrics](operations.md#prometheus-metrics)).
