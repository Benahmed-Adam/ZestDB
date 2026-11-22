# Lua scripting

Scripts add functions and triggers to the database. They are written in Lua 5.4 and run inside the server, in a sandbox.

- **A function** is called by a client with `call <name> "<arg>" ...`. It can run several operations in one round trip.
- **A trigger** runs on a write (`set` or `del`) to the keys it watches. It can run before the write, to clean or reject the value, or after it, for side effects.

Complete scripts for real cases are in [`scripts_examples/`](../scripts_examples/). They can all be loaded together, since no two files register the same name or watch overlapping keys:

| File | Shows |
| --- | --- |
| [`getting_started.lua`](../scripts_examples/getting_started.lua) | `register_function`, arguments, return values, `zest.incr`, `zest.log`, `print` |
| [`validation.lua`](../scripts_examples/validation.lua) | `before` `set` triggers that clean or reject a value, a `before` `del` trigger, `sw` and `re` filters, `zest.caller` |
| [`audit.lua`](../scripts_examples/audit.lua) | `after` `set` / `del` triggers, writes with a TTL, `zest.count`, `zest.caller` |
| [`secondary_index.lua`](../scripts_examples/secondary_index.lua) | `before` + `after` triggers working together, `zest.keys` scans |
| [`bank.lua`](../scripts_examples/bank.lua) | Procedures with checks and compensation |
| [`rate_limit.lua`](../scripts_examples/rate_limit.lua) | A counter with a TTL as a time window |
| [`sessions.lua`](../scripts_examples/sessions.lua) | `zest.setnx`, `zest.expire` |
| [`locks.lua`](../scripts_examples/locks.lua) | `zest.setnx` with a TTL |
| [`maintenance.lua`](../scripts_examples/maintenance.lua) | `zest.keys`, `zest.count`, `zest.ttl`, `zest.persist`, varargs |

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

- **Script names** are 1 to 64 letters, digits, `_` or `-`; the file is `<DbPath>/scripts/<name>.lua`.
- **Loading a name that already exists replaces that script**: its old functions and triggers are removed and the new ones registered. The new version is checked first: if it fails, the old one stays loaded.
- **Function and trigger names are unique** across all scripts. A script that registers a name another script already uses is refused.
- **At startup**, every file of the folder is loaded; a file that fails is logged and skipped.
- **Archives** include the `scripts/` folder, but `restore` only replaces the data: the scripts loaded at that moment are kept.

`script ls` answers `{"scripts": [...], "functions": [...], "triggers": [...]}`, each list sorted by name. `script del <name>` removes the file with its functions and triggers.

`call <function> ["<arg>" ...]` needs the `write` role. Each argument is a quoted value, like the values of `set`.

## API

While a file is being loaded, it may only call `register_function` and `register_trigger`. Reading or writing data is refused at that point, because the file is run once per server thread and on every reload. The functions it registers run later, with the whole API.

| Function | Returns |
| --- | --- |
| `zest.register_function(name, fn)` | Registers `fn` for `call name ...`. The arguments arrive as strings. The first return value becomes the response, converted with `tostring`; `nil` or no value gives an empty response. |
| `zest.register_trigger{ name, phase, event, mode, pattern, fn }` | `phase`: `"before"` or `"after"`. `event`: `"set"` or `"del"`. `mode` and `pattern` go together and are optional; without them the trigger applies to every key. |
| `zest.get(key)` | The value, or `nil` |
| `zest.set(key, value [, ttl])` | Nothing; `ttl` is a positive number of seconds |
| `zest.del(key)` | `true` if the key existed |
| `zest.exists(key)` | `true` / `false` |
| `zest.ttl(key)` | Seconds left, `-1` without a TTL, `-2` if the key does not exist |
| `zest.expire(key, seconds)` / `zest.persist(key)` | `true` if the key exists |
| `zest.incr(key [, n])` | The new value (a missing key counts as 0) |
| `zest.setnx(key, value)` | `true` if the key was created |
| `zest.cas(key, expected, value)` | `true` if the value was replaced |
| `zest.count([mode, pattern])` | The number of live keys |
| `zest.keys([mode, pattern [, lim]])` | A table of keys, sorted; with `lim` (a positive number), the `lim` smallest ones |
| `zest.caller()` | The client that sent the command, as `{ user = "bob", role = "write" }`, or `nil` for a write the server makes on its own. Commands typed in the server's REPL come from `{ user = "ZestLocalAdmin", role = "admin" }`. It cannot be called while loading. |
| `zest.log(message [, level])` / `print(...)` | Writes to the server log. The `level` is `debug`, `info` (default), `warning` or `error`; `print` logs at info. |

`mode` and `pattern` take the same modes as `getby`: `sw`, `ct`, `ew` or `re`.

A failed call (read-only mode, invalid key, value too large, rejected by a trigger...) raises a Lua error whose message is `the command failed with code <code>: <details>`. You can catch it with `pcall`.

When a script stops on an error, the command fails with `600`, `603` or `604`, and `response` holds the error message. For an `error(...)` raised by the script, it starts with the file and line:

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
| `s`, `setex`, `setexat` | ✓ | ✓ |
| `setnx`, `cas` | ✓ | ✓ |
| `setby` / `delby` | ✓ (once per row) | ✓ (once per row) |
| `zest.set` / `zest.del` from a script | ✓ | ✓ |
| `incr` / `decr` | | ✓ (only `after`, since the value is computed under the shard lock) |
| `expire`, `expireat`, `persist`, keys removed by their TTL | | |

Some behaviours to know:

- **When several triggers match a write**, they run in the order they were registered. Each `before` trigger receives the value returned by the previous one.
- **The value a `before` trigger returns is checked again** like any value: it must not be empty, must fit in `MaxValueSize`, and must be valid JSON when `jsonOnly` is enabled.
- **`setby` / `delby` run per row only while at least one trigger is loaded.** They then write and log each key on its own, which is slower than without triggers.
- **A trigger without `mode` and `pattern`** runs Lua on every write of its event.
- **Each write made by a trigger is logged in the WAL** as its own effect. When the WAL is replayed after a crash, the triggers are not run again.
- **An `after` trigger cannot undo the write.** If it fails, the command returns its error, but the write stays.
- **Triggers that keep firing each other stop after 16 levels**, with code `604`.

## Limits and sandbox

- **What scripts can use:** `string` (without `string.dump`), `table`, `math` and `utf8`, plus the base functions. They have no `io`, `os`, `package`, `require`, `debug`, `dofile`, `loadfile` or `collectgarbage`. `load` only accepts text, never bytecode.
- **No clock:** to measure time, use a key's TTL, as [`rate_limit.lua`](../scripts_examples/rate_limit.lua) does.
- **Instructions:** each command may run about 20 million Lua instructions.
- **Memory:** each server thread's Lua state may use 64 MB.
- **What happens past a limit:** the script stops with code `604`.
- **Global variables** are neither shared between server threads nor kept after a reload. Keep any state in the database.
- **A procedure is not a transaction.** Each `zest.*` call is atomic on its own key, but another client can see the state between two calls. [`bank.lua`](../scripts_examples/bank.lua) shows how to undo the steps already done when a check fails.

## Return codes

The full list is in [return codes](return-codes.md).

| Code | Name | When |
| ---: | --- | --- |
| 600 | `ScriptError` | Syntax error, invalid `register_*` call, a name already used by another script, or an error raised while running |
| 601 | `ScriptNotFound` | `script del` / `script reload` of an unknown script |
| 602 | `FunctionNotFound` | `call` of an unknown function |
| 603 | `TriggerRejected` | A `before` trigger refused the write |
| 604 | `ScriptLimitExceeded` | The instruction, memory or recursion limit was reached |
