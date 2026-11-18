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
| `users` | empty | List of `{user, role, tokenHash}` entries (see below). |

The server refuses to start when `MaxValueSize` is not smaller than `SegSize`, when an interval, a timeout or `MaxConnections` is `0`, when `DBPort` equals `WebPort`, when `useSSL` is set without `SSLCertPath` and `SSLKeyPath`, or when a role is unknown. A `MaxKeySize` above 256 is lowered to 256.

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
