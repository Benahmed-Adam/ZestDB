# Lua script examples

Each file is a complete script you can load as is. Together they cover the whole scripting API, on cases met in real applications. They can all be loaded together, and the test suite loads them and runs their scenarios.

| File | Real-world use |
| --- | --- |
| [`getting_started.lua`](getting_started.lua) | The shape of every script; a page counter |
| [`validation.lua`](validation.lua) | Normalised emails, stock that cannot go negative, length limits, admin-only and protected keys |
| [`audit.lua`](audit.lua) | Statistics and an audit trail of who changed what, kept for a week |
| [`secondary_index.lua`](secondary_index.lua) | Find a user by email, a unique email constraint |
| [`bank.lua`](bank.lua) | Deposits, withdrawals and transfers between accounts |
| [`rate_limit.lua`](rate_limit.lua) | Limit an API key to N requests per minute |
| [`sessions.lua`](sessions.lua) | Login sessions with a sliding expiration |
| [`locks.lua`](locks.lua) | A lock shared between workers |
| [`maintenance.lua`](maintenance.lua) | Purge, bulk expiration, inventory, copy and rename |

To load one, copy it to `<DbPath>/scripts/` and run `script reload <name>` (admin role), then call its functions:

```
script reload bank
call deposit "alice" "10000"
```

**[Scripting guide: API, triggers, limits ->](../docs/scripting.md)**
