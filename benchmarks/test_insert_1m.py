#!/usr/bin/env python3
import socket
import hashlib
import time
import json
import struct
import statistics
from concurrent.futures import ThreadPoolExecutor
from threading import Lock

HOST = "localhost"
PORT = 7321
USERNAME = "bob"
PASSWORD = "bob"
NUM_WORKERS = 20
TOTAL_KEYS = 1_000_000
BATCH_SIZE = 5000

results_lock = Lock()
total_inserted = 0
total_errors = 0
start_time_global = 0
all_latencies = []

def send_batch(sock, commands):
    payload = b""
    for cmd in commands:
        cmd_data = cmd.encode('utf-8')
        payload += struct.pack('>I', len(cmd_data)) + cmd_data
    total_size = struct.pack('>I', len(payload))
    sock.sendall(total_size + payload)

def recv_responses(sock, num_commands, buffer="", timeout=30.0):
    sock.settimeout(timeout)
    try:
        while True:
            parts = buffer.split('\r\n\r\n')
            if len(parts) >= num_commands and '\n' in parts[-1]:
                break
            data = sock.recv(4096).decode('utf-8')
            if not data:
                return None, ""
            buffer += data
        parts = buffer.split('\r\n\r\n', num_commands - 1)
        buffer = ""
        return [p.strip('\r\n') for p in parts], buffer
    except socket.timeout:
        return None, buffer

def sha256(data: str) -> str:
    return hashlib.sha256(data.encode()).hexdigest()

def calculate_percentile(data: list, percentile: float) -> float:
    if not data:
        return 0
    data_sorted = sorted(data)
    n = len(data_sorted)
    index = (percentile / 100.0) * (n - 1)
    lower = int(index)
    upper = lower + 1
    if upper >= n:
        return data_sorted[-1]
    weight = index - lower
    return data_sorted[lower] * (1 - weight) + data_sorted[upper] * weight


def worker_insert_batched(worker_id: int):
    global total_inserted, total_errors, all_latencies

    keys_per_worker = TOTAL_KEYS // NUM_WORKERS
    start_key = worker_id * keys_per_worker

    try:
        sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        sock.settimeout(30.0)
        sock.connect((HOST, PORT))

        buffer = ""

        token = sha256(USERNAME + PASSWORD)
        send_batch(sock, [f"Authorization: {USERNAME}.{token}"])
        responses, buffer = recv_responses(sock, 1, buffer, timeout=10.0)

        if responses is None or ("OK:" not in responses[0] and "authenticated" not in responses[0].lower()):
            print(f"  Worker {worker_id}: auth failed")
            sock.close()
            with results_lock:
                total_errors += 1
            return

        local_latencies = []
        local_inserted = 0
        local_errors = 0

        commands_batch = []

        for i in range(keys_per_worker):
            key_num = start_key + i
            key = f"pipe_{worker_id}_{key_num}"
            value = json.dumps({"id": key_num, "worker": worker_id})
            cmd = f"set {key} {value}"

            commands_batch.append(cmd)

            if len(commands_batch) == BATCH_SIZE or i == keys_per_worker - 1:
                req_start = time.perf_counter()

                send_batch(sock, commands_batch)
                responses, buffer = recv_responses(sock, len(commands_batch), buffer)

                latency_per_cmd = (time.perf_counter() - req_start) * 1000 / len(commands_batch)

                local_latencies.extend([latency_per_cmd] * len(commands_batch))

                if responses is None:
                    local_errors += len(commands_batch)
                else:
                    for resp in responses:
                        if "OK:" in resp:
                            local_inserted += 1
                        else:
                            local_errors += 1

                commands_batch = []

            if i > 0 and i % 5000 == 0:
                with results_lock:
                    total_inserted += local_inserted
                    total_errors += local_errors
                    all_latencies.extend(local_latencies)
                    current = total_inserted

                elapsed = time.time() - start_time_global
                rate = current / elapsed if elapsed > 0 else 0
                print(f"  Worker {worker_id}: {i:,}/{keys_per_worker:,} ({rate:,.0f} ops/s)")

                local_latencies = []
                local_inserted = 0
                local_errors = 0

        with results_lock:
            total_inserted += local_inserted
            total_errors += local_errors
            all_latencies.extend(local_latencies)

        sock.close()
        print(f"  Worker {worker_id} done: {local_inserted} inserted, {local_errors} errors")

    except Exception as e:
        print(f"  Worker {worker_id} error: {e}")
        with results_lock:
            total_errors += 1


def check_server():
    try:
        sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        sock.settimeout(5.0)
        sock.connect((HOST, PORT))

        buffer = ""
        token = sha256(USERNAME + PASSWORD)
        send_batch(sock, [f"Authorization: {USERNAME}.{token}"])
        responses, buffer = recv_responses(sock, 1, buffer)

        sock.close()

        if responses and ("OK:" in responses[0] or "authenticated" in responses[0].lower()):
            print(f"  Server OK on {HOST}:{PORT}")
            return True
    except Exception as e:
        print(f"  Server check failed: {e}")
        return False

    return False


def run_insert_test():
    global total_inserted, total_errors, start_time_global, all_latencies

    print("\n" + "=" * 70)
    print("  1 MILLION KEYS INSERT TEST (TRUE BATCHING)")
    print("=" * 70)
    print(f"  Workers: {NUM_WORKERS}")
    print(f"  Total keys: {TOTAL_KEYS:,}")
    print(f"  Batch size: {BATCH_SIZE}")
    print(f"  Target: {HOST}:{PORT}")
    print("=" * 70)

    if not check_server():
        print("\n  ERROR: Server not available!")
        return

    total_inserted = 0
    total_errors = 0
    all_latencies = []

    print(f"\n  Starting insertion...")
    start_time_global = time.time()

    with ThreadPoolExecutor(max_workers=NUM_WORKERS) as executor:
        list(executor.map(worker_insert_batched, range(NUM_WORKERS)))

    end_time = time.time()
    duration = end_time - start_time_global

    print("\n" + "=" * 70)
    print("  Result:")
    print(f"    Duration: {duration:.2f}s")
    print(f"    Success: {total_inserted:,}")
    if duration > 0:
        print(f"    Throughput: {total_inserted / duration:.2f} req/s")

    if all_latencies:
        lat_sorted = sorted(all_latencies)
        p95 = calculate_percentile(lat_sorted, 95)
        print(f"    Latency min: {min(lat_sorted):.3f} ms")
        print(f"    Latency avg: {statistics.mean(lat_sorted):.2f} ms")
        print(f"    Latency P95: {p95:.2f} ms")
        print(f"    Latence max: {max(lat_sorted):.2f} ms")

    print(f"    Errors: {total_errors}")
    print("=" * 70)


if __name__ == "__main__":
    import sys

    if len(sys.argv) > 1 and sys.argv[1] == "cleanup":
        print("  Cleanup not implemented")
    else:
        run_insert_test()