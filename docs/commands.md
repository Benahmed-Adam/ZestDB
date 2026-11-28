# Commands

Commands are whitespace-delimited and case-insensitive, with short aliases (`s`, `g`, `d`...). Values are always written between double quotes, so they may contain spaces: `s greeting "hello world"`.

Every command returns one JSON object with `code`, `response` and `affectedRows` (see [the protocol](protocol.md#response-format) and [the return codes](return-codes.md)).

## Reference

| Command | Description |
| --- | --- |
| `get <key>` / `g <key>` | Retrieve the value stored for a key. |
| `set <key> "<value>"` / `s <key> "<value>"` | Store a value. |
| `del <key>` / `d <key>` | Delete a key (marks a tombstone). |
| `setex <key> <seconds> "<value>"` | Store a value that expires after `<seconds>`. |
| `expire <key> <seconds>` / `persist <key>` | Set or remove the TTL of an existing key. |
| `setexat <key> <ms> "<value>"` / `expireat <key> <ms>` | Same as `setex` / `expire` with an absolute expiry date, in milliseconds since the Unix epoch (the form written to the WAL). |
| `ttl <key>` | Seconds before the key expires (`-1`: no TTL, `-2`: no key). |
| `exists <key>` | `true` if the key exists and has not expired, `false` otherwise (`affectedRows` is `1` or `0`). |
| `count [<mode> <pattern>]` | Number of live keys, or of the keys matching the pattern (same modes as `getby`), without reading any value. |
| `keys [<mode> <pattern>] [lim <n>]` | Live keys as a sorted JSON array, or the keys matching the pattern; with `lim`, the `n` smallest ones (e.g. `keys sw user: lim 100`). Values are not read. |
| `setnx <key> "<value>"` | Store a value only if the key does not exist (an expired key counts as absent); otherwise returns code `207`. |
| `cas <key> "<expected>" "<new>"` | Atomically replace the value only if it equals `<expected>`, e.g. `cas balance "100" "80"`; otherwise returns code `208`. The TTL is kept. |
| `incr <key> [n]` / `decr <key> [n]` | Atomically add or subtract `n` (default 1) to an integer value and return the result; a missing key counts as 0, the TTL is kept. |
| `getby <mode> <pattern> [lim <n>]` / `gb ...` | Get all entries whose key matches the rule. |
| `setby <mode> <pattern> "<value>" [lim <n>]` / `sb ...` | Update the value of all matching keys; each key keeps its TTL. With `lim`, the `n` smallest matching keys. |
| `delby <mode> <pattern> [lim <n>]` / `db ...` | Delete all matching keys. |
| `script load <name> "<lua code>"` | Save a Lua script and register its functions and triggers (see [scripting](scripting.md)). |
| `script reload [<name>]` | Reload `<DbPath>/scripts/<name>.lua` after editing it on disk, or every script of the folder without a name. |
| `script del <name>` / `script ls` | Remove a script with its functions and triggers / list the scripts, functions and triggers as JSON. |
| `call <function> ["<arg>" ...]` | Run a function registered by a script; its return value is the response. |
| `flush` / `f` | Force all in-memory data to disk and clear the WAL. |
| `reload` / `r` | Reload the configuration from `config.yaml`. |
| `scfg <param> <value>` | Set a configuration parameter at runtime. |
| `gcfg` | Print the full active configuration as JSON. |
| `save` | Manually create a zip archive of the database. |
| `restore <archive.zip>` | Replace the database with an archive from `ArchiveStoragePath` (e.g. `restore zestdb_archive_2026-11-15_10-00-00.123456789.zip`). Everything written since that archive is lost. |
| `lsaves` | List the archives of `ArchiveStoragePath` as a JSON array, oldest first (e.g. `["zestdb_archive_2026-11-15_10-00-00.123456789.zip"]`); `affectedRows` is their number. |
| `info` / `stats` | Database summary as JSON, e.g. `{"cacheHitRatio":0.93,"diskBytes":1048576,"keys":1200,"lastArchive":{"date":"2026-11-15T10:00:00Z","name":"zestdb_archive_2026-11-15_10-00-00.123456789.zip"}}`. `keys` counts expired keys until the next compaction drops them; `diskBytes` is the size of every file of `DbPath`; `cacheHitRatio` is the share of `get` / `getby` served from the cache since startup (`null` before the first read); `lastArchive` is `null` without an archive. `replication` is `{"role":"primary","historyId","nextSeq","replicas"}` on a primary (`nextSeq`: the sequence number of its next write; `replicas`: the replicas streaming from it), and `{"role":"replica","primary","connected","nextSeq"}` on a replica (`nextSeq`: the next write it expects). A replica's lag is its primary's `nextSeq` minus its own. |
| `user add <name> "<password>" [role]` | Create a user; the role is `read`, `write` (the default) or `admin` (see [managing users](configuration.md#managing-users-at-runtime)). |
| `user passwd <name> "<password>"` | Change a user's password and close its open connections. |
| `user role <name> <role>` | Change a user's role; its open connections get it from their next command. |
| `user del <name>` | Delete a user and close its open connections. |
| `user ls` | List the users and their roles as a JSON array, sorted by name (e.g. `[{"role":"admin","user":"root"}]`). |
| `help` / `h` | Print command and usage help. |

`script`, `user`, `scfg`, `reload`, `gcfg`, `save`, `restore`, `lsaves` and `flush` require the `admin` role; `call` and the commands that write require the `write` role (see [roles](configuration.md#users-and-roles)). `useSSL`, the ports and the connection limits can only be changed in `config.yaml`, followed by a restart.

## Values

- Keys and values are strings. The key length is limited by `MaxKeySize`, the value length by `MaxValueSize`.
- With `jsonOnly`, any value that is not valid JSON is rejected; with `readOnly`, every write is blocked while reads still work.

### Quoted values

- A value starts and ends with `"`. Inside it, `\"` stands for a quote and `\\` for a backslash; any other backslash is kept as is.
- Nothing may follow the closing quote, except `lim <n>` for `setby`: `sb sw order: "paid lim 5" lim 10` stores `paid lim 5` in at most 10 keys.
- A value that is not quoted, not terminated or followed by anything else is rejected with code `113`; an empty value `""` with code `203`.

```
zestdb> s user "{\"name\": \"Bob\", \"path\": \"C:\\\\tmp\"}"
{"affectedRows":1,"code":0,"response":""}
zestdb> g user
{"affectedRows":1,"code":0,"response":{"name": "Bob", "path": "C:\\tmp"}}
```
A stored value that is valid JSON is embedded as is in the response; any other value is returned as a JSON string.

## Expiration (TTL)

- `setex`, `expire`, `ttl` and `persist` give keys a time to live; the expiry is stored as an absolute date in the index.
- Expired keys are invisible to every command and are dropped at the next compaction.

## Atomic operations

- `incr` / `decr` on integer values, `setnx` (write only if the key is absent) and `cas` (compare and swap).
- Each one reads and writes under the shard's exclusive lock, so concurrent clients never lose an update.

## Pattern commands

`getby`, `setby`, `delby`, `count` and `keys` select keys with a matching rule:

- `re` - regex match on the full key (ECMAScript syntax via `std::regex`).
- `sw` - key starts with the pattern.
- `ct` - key contains the pattern.
- `ew` - key ends with the pattern.
- `lim <n>` - global cap on the number of matched/affected entries, enforced across all shards.

Bulk operations are executed in parallel across shards and the results are merged.

## Example session

```
zestdb> s name "Bob"
{"affectedRows":1,"code":0,"response":""}
zestdb> g name
{"affectedRows":1,"code":0,"response":"Bob"}
zestdb> gb sw na
{"affectedRows":1,"code":0,"response":[{"name":"Bob"}]}
zestdb> scfg cachesize 500
{"affectedRows":0,"code":0,"response":""}
zestdb> d name
{"affectedRows":1,"code":0,"response":""}
zestdb> g name
{"affectedRows":0,"code":200,"response":""}
```

## `scfg` parameters

Changes made with `scfg` apply at once and are written back to `config.yaml`.

| Parameter | Type | Notes |
| --- | --- | --- |
| `SegSize` | int | Must be greater than `MaxValueSize`. |
| `MaxKeySize` | int | Cannot exceed the internal limit (256). |
| `MaxValueSize` | int | Must be less than `SegSize`. |
| `CacheSize` | int | Per-shard cache capacity. |
| `CompactingInterval` | int | Maximum seconds between two compactions of a shard; a shard is compacted earlier once half of its data is dead. |
| `FlushInterval` | int | Seconds between disk syncs. |
| `NetworkValidationStr` | string | IP allow-list regex. |
| `isDebug` | bool | Enable debug logging (`true/false` or `1/0`). |
| `jsonOnly` | bool | Reject non-JSON values. |
| `readOnly` | bool | Block write operations. |
| `ArchiveCreationDelay` | int | Seconds between automatic archives. |
| `AutoArchiveSaving` | bool | Enable/disable automatic archiving. |
