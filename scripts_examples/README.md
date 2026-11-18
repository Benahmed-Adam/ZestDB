# ZestDB Lua script examples

Each file is a complete script you can load as is. Together they cover every part of the scripting API, on cases met in real applications.

| File | Shows | Real-world use |
| --- | --- | --- |
| [`getting_started.lua`](getting_started.lua) | `register_function`, arguments, return values, `zest.incr`, `zest.log`, `print` | The shape of every script; a page counter |
| [`validation.lua`](validation.lua) | `before` `set` triggers that clean or reject a value, a `before` `del` trigger, `sw` and `re` filters | Normalised emails, stock that cannot go negative, length limits, protected keys |
| [`audit.lua`](audit.lua) | `after` `set` / `del` triggers, writes with a TTL, `zest.count` | Statistics and an audit trail kept for a week |
| [`secondary_index.lua`](secondary_index.lua) | `before` + `after` triggers working together, `zest.keys` scans | Find a user by email, a unique email constraint |
| [`bank.lua`](bank.lua) | Procedures with checks and compensation | Deposits, withdrawals and transfers between accounts |
| [`rate_limit.lua`](rate_limit.lua) | A counter with a TTL as a time window | Limit an API key to N requests per minute |
| [`sessions.lua`](sessions.lua) | `zest.setnx`, `zest.expire` | Login sessions with a sliding expiration |
| [`locks.lua`](locks.lua) | `zest.setnx` with a TTL | A lock shared between workers |
| [`maintenance.lua`](maintenance.lua) | `zest.keys`, `zest.count`, `zest.ttl`, `zest.persist`, varargs | Purge, bulk expiration, inventory, copy and rename |

They can all be loaded together: no two files register the same name or watch overlapping keys.

## Loading a script

Two ways, both reserved to the `admin` role:

- **Copy the file** to `<DbPath>/scripts/<name>.lua`, then run `script reload <name>`. Every file in that folder is also loaded when the server starts.
  - `script reload` without a name reloads the whole folder: edited files are reloaded, new files loaded and deleted files unloaded.
  - It answers `{"loaded": [...], "failed": [...]}`. A file that fails is logged and skipped, and the others still load.
- **Send it** with `script load <name> "<code>"`. The code is a quoted value: escape `"` as `\"` and `\` as `\\`.

```
script load bank "zest.register_function(\"balance\", function(account) return zest.get(\"balance:\" .. account) or \"0\" end)"
script ls
call balance "alice"
script del bank
```

`script ls` returns the scripts, functions and triggers as JSON. `call <function> ["<arg>" ...]` needs the `write` role.

## API

While a file is being loaded, it may only call `register_function` and `register_trigger`. Reading or writing data is refused at that point, because the file is run once per server thread and on every reload. The functions it registers run later, with the whole API.

| Function | Returns |
| --- | --- |
| `zest.register_function(name, fn)` | Registers `fn` for `call name ...`. The arguments arrive as strings; the return value becomes the response. |
| `zest.register_trigger{ name, phase, event, mode, pattern, fn }` | `phase`: `"before"` or `"after"`. `event`: `"set"` or `"del"`. `mode` and `pattern` go together and are optional; without them the trigger applies to every key. |
| `zest.get(key)` | The value, or `nil` |
| `zest.set(key, value [, ttl])` | Nothing; `ttl` is in seconds |
| `zest.del(key)` | `true` if the key existed |
| `zest.exists(key)` | `true` / `false` |
| `zest.ttl(key)` | Seconds left, `-1` without a TTL, `-2` if the key does not exist |
| `zest.expire(key, seconds)` / `zest.persist(key)` | `true` if the key exists |
| `zest.incr(key [, n])` | The new value (a missing key counts as 0) |
| `zest.setnx(key, value)` | `true` if the key was created |
| `zest.cas(key, expected, value)` | `true` if the value was replaced |
| `zest.count([mode, pattern])` | The number of live keys |
| `zest.keys([mode, pattern [, lim]])` | A table of keys, sorted |
| `zest.log(message [, level])` / `print(...)` | Writes to the server log. The `level` is `debug`, `info` (default), `warning` or `error`; `print` logs at info. |

A failed call (read-only mode, invalid key, value too large...) raises a Lua error. You can catch it with `pcall`.

When a script stops on an error, the command fails with `600`, `603` or `604`, and `response` holds the error message, with its file and line:

```json
{"affectedRows":0,"code":603,"response":"validation.lua:24: invalid email for email:2"}
```

## Triggers

| Trigger | Receives | Effect |
| --- | --- | --- |
| `before` `set` | `(key, value)` | Must return the value to store, the original or a changed one. Raising an error rejects the write with code `603`. |
| `before` `del` | `(key)` | Raising an error rejects the delete with code `603`. |
| `after` `set` | `(key, value)` | Runs once the final value is stored. |
| `after` `del` | `(key)` | Runs once the key is deleted. |

Triggers run for these commands:

| Command | `before` | `after` |
| --- | --- | --- |
| `s`, `setex` | ✓ | ✓ |
| `setnx`, `cas` | ✓ | ✓ |
| `setby` / `delby` | ✓ (once per row) | ✓ (once per row) |
| `zest.set` / `zest.del` from a script | ✓ | ✓ |
| `incr` / `decr` | | ✓ (only `after`, since the value is computed under the shard lock) |
| `expire`, `persist`, keys removed by their TTL | | |

Some behaviours to know:

- **Each write made by a trigger is logged in the WAL** as its own effect. When the WAL is replayed after a crash, the triggers are not run again.
- **An `after` trigger cannot undo the write.** If it fails, the command returns its error, but the write stays.
- **Triggers that keep firing each other stop after 16 levels**, with code `604`.

## Limits and sandbox

- **What scripts can use:** `string` (without `string.dump`), `table`, `math` and `utf8`, plus the base functions. They have no `io`, `os`, `package`, `require`, `debug`, `dofile`, `loadfile` or `collectgarbage`. `load` only accepts text, never bytecode.
- **No clock:** to measure time, use a key's TTL, as [`rate_limit.lua`](rate_limit.lua) does.
- **Instructions:** each command may run about 20 million Lua instructions.
- **Memory:** each server thread's Lua state may use 64 MB.
- **What happens past a limit:** the script stops with code `604`.
- **Global variables** are neither shared between server threads nor kept after a reload. Keep any state in the database.
- **A procedure is not a transaction.** Each `zest.*` call is atomic on its own key, but another client can see the state between two calls. [`bank.lua`](bank.lua) shows how to undo the steps already done when a check fails.

## Return codes

The full list is in [`returncodes.md`](../returncodes.md).

| Code | Name | When |
| ---: | --- | --- |
| 600 | `ScriptError` | Syntax error, invalid `register_*` call, a name already used by another script, or an error raised while running |
| 601 | `ScriptNotFound` | `script del` / `script reload` of an unknown script |
| 602 | `FunctionNotFound` | `call` of an unknown function |
| 603 | `TriggerRejected` | A `before` trigger refused the write |
| 604 | `ScriptLimitExceeded` | The instruction, memory or recursion limit was reached |
