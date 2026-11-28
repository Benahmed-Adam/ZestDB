# Network protocol

Clients talk to the TCP server (`DBPort`, default `7321`) using a length-prefixed, big-endian framing. See [`benchmark/zest_client.py`](../benchmark/zest_client.py) for a minimal reference client.

## Server

- Custom asynchronous TCP server built on Asio, with one `io_context` per CPU core: each session stays on one thread, so requests are never handed between threads. Batched commands are acknowledged after a single WAL write (group commit).
- Optional TLS/SSL (`useSSL`), for both the TCP server and the HTTP stats server.
- Configurable IP allow-listing using a regular expression (`NetworkValidation`), applied to both servers.
- Connection limits: `MaxConnections` simultaneous sessions, `AuthTimeout` to finish the TLS handshake and authenticate, `IdleTimeout` for authenticated sessions.
- Optional per-user rate limits (commands per second, request bytes per second); a refused command is answered with code `304` (see [rate limits](configuration.md#rate-limits)).

## Frame format

```
4 bytes      payload length (network byte order)
N bytes      concatenated commands
```
Each command within the payload is itself length-prefixed:
```
4 bytes      command length (network byte order)
M bytes      command string
```
A single request can therefore carry several commands, with one JSON response per command.

Before authentication a request payload is limited to 1 MiB, and to 64 MiB afterwards; a larger size closes the connection.

## Authentication

A session must first send:
```
Authorization: <username>.<sha256(username+password)>
```
The server replies with `{"code":0,...}` on success; on failure it replies with code `300` and closes the connection. Any other command sent before authentication gets the same answer: code `300`, then the connection is closed. With no user in `config.yaml`, no client can authenticate. Each user has a role, checked on every command (see [users and roles](configuration.md#users-and-roles)). Deleting a user or changing its password (`user del`, `user passwd`, or a `reload`) closes its open connections: an idle one at once, a busy one after its current command, the rest of the batch being answered with code `302`.

A user is locked out for 60 seconds after 5 failed authentications. The lock is per user rather than per address, so it still works behind a proxy such as ngrok (where every client comes from `127.0.0.1`); the flip side is that anyone can lock a known user out for a minute.

Without `useSSL` the token travels in clear and a captured token can be replayed, so enable SSL outside a trusted network.

## Replication handshake

A replica opens an ordinary session, authenticates as a user that has the `replica` role, then sends:
```
replicate <historyId> <fromSeq>
```
- `historyId` is the history the replica last synced with (`-` for none), and `fromSeq` the sequence number of the first write it still needs.
- The primary refuses the replica with code `310` if its address does not match `allowedReplicas`, and with code `311` if it is itself a replica. Any other user gets code `301`.
- On success, the response is `{"mode":"stream"|"fullsync","historyId":"...","nextSeq":N}`:
  - `stream` when the primary still holds every write of that history from `fromSeq` on.
  - `fullsync` otherwise: a new replica, a primary that restarted (each run has a new `historyId`, since the sequence numbers start over once the WAL is cleared), or a replica that fell too far behind (`historySize`).
- The session then stays open for the replication stream, and the idle timeout no longer applies to it. The replica sends nothing more on it: any request closes the link.

The `replica` role runs no other command (code `301`).

## Replication stream

In `stream` mode, the primary pushes frames on the link, right after the handshake response (a JSON line ending with `\n`). A frame has the request framing:
```
4 bytes      frame length (network byte order)
N bytes      events, each a 4-byte length followed by `<seq> <command>`
```
- The events come in sequence order, from `fromSeq` on, up to 1024 per frame. The sequence numbers may skip a number, taken by a write whose WAL append failed.
- The commands are the WAL records: the effect of each write (`s`, `setexat`, `expireat`, `persist`, `d`), with one record per key for `setby` / `delby`.
- An empty frame (length `0`) is a heartbeat, sent every second when there is nothing else: a replica that gets nothing for a few seconds can take the link as dead.
- An event is sent once it is in the primary's WAL buffer, possibly before the batch's WAL sync. If the primary crashes in between, the replica may hold a write the primary lost; the primary's next run has a new `historyId`, so the replica then gets a full sync.
- A replica that falls more than `historySize` writes behind is disconnected; at its next handshake it gets `fullsync`.

## Full sync

In `fullsync` mode, the primary first archives its database, as `save` does (a flush, then a zip, while writes wait), into `<ArchiveStoragePath>/.replication/`. It then sends:
1. A frame holding one item, `<seq> snapshot <bytes>`: the archive's size, and the sequence number of the first write it does not hold.
2. The `<bytes>` of the zip archive, raw, without framing.
3. The replication stream from `<seq>` on, as in `stream` mode.

The archive file is deleted once it is sent, or when the link closes. If another archive or a restore is in progress, the primary closes the link and the replica retries later. A `restore` on the primary draws a new `historyId` and closes every replication link, so each replica gets a full sync at its next handshake.

## Response format

Each command yields one JSON object:
```json
{"affectedRows":1,"code":0,"response":"Bob"}
```
- `code`: `0` on success, otherwise the error code. Every code is listed in [return codes](return-codes.md).
- `response`: the data returned by the command (a string, or a JSON array/object for bulk operations); empty when the command returns no data, and on errors, except script errors, where it holds the error message.
- `affectedRows`: number of affected entries.

Multiple commands in one request are answered with responses separated by `\r\n\r\n` and terminated by a trailing newline.
