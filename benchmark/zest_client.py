import argparse
import hashlib
import json
import socket
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
    def __init__(self, host: str, port: int, user: str, password: str):
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self.sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        self.sock.connect((host, port))
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


def count_errors(reply: bytes) -> int:
    return reply.count(b'"code":1')


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
    p.add_argument("--json", metavar="PATH", help="also write the results to this JSON file")
    return p


def connect(args: argparse.Namespace) -> ZestClient:
    return ZestClient(args.host, args.port, args.user, args.password)


def int_list(text: str) -> list[int]:
    return [int(x.replace("_", "")) for x in text.split(",")]


def write_json(path: str | None, results: list[dict]) -> None:
    if path:
        with open(path, "w", encoding="utf-8") as f:
            json.dump(results, f, indent=2)
        print(f"Results written to {path}")

