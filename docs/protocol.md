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

## Response format

Each command yields one JSON object:
```json
{"affectedRows":1,"code":0,"response":"Bob"}
```
- `code`: `0` on success, otherwise the error code. Every code is listed in [return codes](return-codes.md).
- `response`: the data returned by the command (a string, or a JSON array/object for bulk operations); empty when the command returns no data, and on errors, except script errors, where it holds the error message.
- `affectedRows`: number of affected entries.

Multiple commands in one request are answered with responses separated by `\r\n\r\n` and terminated by a trailing newline.
