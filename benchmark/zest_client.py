import argparse
import hashlib
import json
import re
import socket
import ssl
import statistics
import struct
import sys
import time


def sha256_hex(s: str) -> str:
    return hashlib.sha256(s.encode()).hexdigest()


def encode_cmd(cmd: str) -> bytes:
    raw = cmd.encode()
    return struct.pack("!I", len(raw)) + raw


def build_frame(commands: list[str]) -> bytes:
    inner = b"".join(encode_cmd(c) for c in commands)
    return struct.pack("!I", len(inner)) + inner


class ZestClient:
    def __init__(self, host: str, port: int, user: str, password: str, use_ssl: bool = False):
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self.sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        self.sock.connect((host, port))
        if use_ssl:
            # Encrypted but unverified: the server usually runs with a self-signed cert.pem
            ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
            ctx.check_hostname = False
            ctx.verify_mode = ssl.CERT_NONE
            self.sock = ctx.wrap_socket(self.sock, server_hostname=host)
        resp = json.loads(self.send([f"Authorization: {user}.{sha256_hex(user + password)}"]))
        if resp.get("code") != 0:
            self.close()
            sys.exit(f"Authentication failed: {resp}")

    def send(self, commands: list[str]) -> bytes:
        """Send one framed batch and return the raw reply (one JSON object per command)."""
        self.sock.sendall(build_frame(commands))
        buf = bytearray()
        while not buf.endswith(b"}\n"):
            chunk = self.sock.recv(1 << 20)
            if not chunk:
                raise ConnectionError("Server closed the connection")
            buf += chunk
        return bytes(buf)

    def timed(self, commands: list[str]) -> tuple[float, bytes]:
        """Send a batch, return (latency in ms, raw reply)."""
        t0 = time.perf_counter()
        reply = self.send(commands)
        return (time.perf_counter() - t0) * 1000.0, reply

    def close(self) -> None:
        self.sock.close()


def count_errors(reply: bytes, expected: int = 0) -> int:
    """Count the responses whose code is not the expected one (some scenarios expect a refusal, e.g. 207)."""
    return sum(1 for code in re.findall(rb'"code":(\d+)', reply) if int(code) != expected)


def summarize(latencies_ms: list[float]) -> dict:
    s = sorted(latencies_ms)
    n = len(s)
    return {
        "count": n,
        "avg": statistics.mean(s),
        "med": statistics.median(s),
        "p95": s[min(n - 1, int(n * 0.95))],
        "p99": s[min(n - 1, int(n * 0.99))],
        "best": s[0],
        "worst": s[-1],
    }


def base_parser(description: str) -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(description=description)
    p.add_argument("--host", default="127.0.0.1")
    p.add_argument("--port", type=int, default=7321)
    p.add_argument("--user", default="bob")
    p.add_argument("--password", default="bob")
    p.add_argument("--ssl", action="store_true", help="connect over TLS (server started with useSSL: true); the certificate is not verified")
    p.add_argument("--json", metavar="PATH", help="also write the results to this JSON file")
    return p


def conn_args(args: argparse.Namespace) -> tuple:
    """Plain values (picklable for worker processes) to pass to ZestClient(*conn_args(args))."""
    return (args.host, args.port, args.user, args.password, args.ssl)


def connect(args: argparse.Namespace) -> ZestClient:
    return ZestClient(*conn_args(args))


def int_list(text: str) -> list[int]:
    return [int(x.replace("_", "")) for x in text.split(",")]


def write_json(path: str | None, results: list[dict]) -> None:
    if path:
        with open(path, "w", encoding="utf-8") as f:
            json.dump(results, f, indent=2)
        print(f"Results written to {path}")

