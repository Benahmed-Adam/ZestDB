# Return codes

Every response carries a numeric `code`: `0` on success, otherwise one of the error codes below. On an error, `response` is always empty.

```json
{"affectedRows":0,"code":200,"response":""}
```

The values are part of the protocol: a code is never renumbered or reused. They come from `ResultType::Code` in `src/Settings.hpp`, and this file must be updated with it.

| Range | Family |
| --- | --- |
| `0` | Success |
| `1xx` | Malformed command or request |
| `2xx` | Key or value |
| `3xx` | Authentication and permissions |
| `4xx` | Configuration |
| `5xx` | Server-side failure |

## 0 - Success

| Code | Name | Meaning |
| ---: | --- | --- |
| 0 | `Success` | The command succeeded. `response` holds the data it returns (value, `getby` results, `ttl`, `gcfg`, `help`), or is empty. `affectedRows` counts the entries read or modified. |

## 1xx - Malformed command or request

| Code | Name | Meaning |
| ---: | --- | --- |
| 100 | `NoCommand` | The command is empty or only whitespace. |
| 101 | `UnknownCommand` | The command name does not exist. |
| 102 | `MissingKey` | The command needs a key. |
| 103 | `MissingValue` | The command needs a value. |
| 104 | `MissingPattern` | `delby` needs a mode and a pattern. |
| 105 | `MissingArguments` | Not enough arguments (`getby`, `expire`, `scfg`...). |
| 106 | `InvalidMode` | `delby` mode is not `re`, `sw`, `ct` or `ew`. |
| 107 | `InvalidRegex` | `getby` / `setby`: invalid mode, or regex invalid, too complex or using back-references. |
| 108 | `InvalidLimit` | `lim` is not a positive integer. |
| 109 | `InvalidTtl` | The TTL is not a positive integer (`setex`, `expire`, `setexat`, `expireat`). |
| 110 | `MalformedBatch` | A command length in the batch frame does not match its content. |
| 111 | `PayloadTooLarge` | The request exceeds 1 MiB before authentication or 64 MiB after; the connection is closed. |
| 112 | `InvalidIncrement` | The increment of `incr` / `decr` is not an integer. |
| 113 | `InvalidValueFormat` | A value is not between double quotes, its closing quote is missing, or something other than `lim <n>` (for `setby`) follows it. |

## 2xx - Key or value

| Code | Name | Meaning |
| ---: | --- | --- |
| 200 | `KeyNotFound` | The key does not exist, was deleted or has expired. |
| 201 | `InvalidKey` | The key is longer than `MaxKeySize`. |
| 202 | `InvalidValue` | The value is longer than `MaxValueSize`. |
| 203 | `EmptyValue` | The value is empty. |
| 204 | `JsonOnly` | `jsonOnly` is enabled and the value is not valid JSON. |
| 205 | `NotAnInteger` | `incr` / `decr`: the stored value is not a 64-bit integer; it is left unchanged. |
| 206 | `IntegerOverflow` | `incr` / `decr`: the result would leave the 64-bit integer range; the value is left unchanged. |
| 207 | `KeyExists` | `setnx`: the key already exists; its value is left unchanged. |
| 208 | `ValueMismatch` | `cas`: the current value differs from the expected one; it is left unchanged. |

## 3xx - Authentication and permissions

| Code | Name | Meaning |
| ---: | --- | --- |
| 300 | `AuthFailed` | Wrong user or token, malformed `Authorization` line, or the user is locked out after too many failures; the connection is closed. |
| 301 | `PermissionDenied` | The user's role does not allow this command. |
| 302 | `SessionRevoked` | The user was removed from the configuration; the connection is closed. |
| 303 | `ReadOnly` | `readOnly` is enabled and the command writes. |

## 4xx - Configuration

| Code | Name | Meaning |
| ---: | --- | --- |
| 400 | `UnknownConfigParam` | `scfg` parameter does not exist. |
| 401 | `InvalidConfigValue` | `scfg` value is invalid or out of range (e.g. `SegSize` not greater than `MaxValueSize`, `MaxKeySize` above 256, interval of 0, invalid regex). |
| 402 | `ConfigNotReloadable` | The setting cannot change at runtime (paths, ports, SSL, connection limits): edit `config.yaml` and restart. |
| 403 | `ReloadFailed` | `reload` could not read or parse `config.yaml`; the current configuration is kept. |

## 5xx - Server-side failure

| Code | Name | Meaning |
| ---: | --- | --- |
| 500 | `InternalError` | The command raised an unexpected exception. |
| 501 | `ReadFailed` | The value could not be read from its data segment. |
| 502 | `WriteFailed` | The value could not be written to a data segment. |
| 503 | `WalWriteFailed` | The write was applied but could not be logged in the WAL. |
| 504 | `ArchiveFailed` | `save` could not create the archive (or one is already in progress). |
