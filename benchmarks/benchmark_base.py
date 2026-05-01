import socket
import hashlib
import time
import statistics
import random
import psutil
import json
import struct
from concurrent.futures import ThreadPoolExecutor
from threading import Lock
from threading import Thread

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

def send_command(sock, cmd):
    send_batch(sock, [cmd])

def recv_response(sock, buffer="", timeout=30.0):
    responses, buffer = recv_responses(sock, 1, buffer, timeout)
    return responses[0] if responses else None, buffer

HOST = "localhost"
PORT = 7321
USERNAME = "bob"
PASSWORD = "bob"
CONCURRENT_USERS = 50
REQUESTS_PER_USER = 20000

stats_lock = Lock()
all_latencies = []
total_errors = 0
total_success = 0

monitor_lock = Lock()
cpu_samples = []
mem_samples = []
stop_monitoring = False

def find_zestdb_process():
    for proc in psutil.process_iter(['name', 'pid']):
        try:
            if proc.info['name'] and 'zestdb' in proc.info['name'].lower():
                return proc
        except (psutil.NoSuchProcess, psutil.AccessDenied):
            continue
    return None

def monitor_resources(process_name="zestdb"):
    global stop_monitoring, cpu_samples, mem_samples

    while not stop_monitoring:
        proc = find_zestdb_process()
        if proc:
            try:
                cpu = proc.cpu_percent(interval=0.1)
                mem = proc.memory_info().rss / (1024 * 1024)
                with monitor_lock:
                    cpu_samples.append(cpu)
                    mem_samples.append(mem)
            except (psutil.NoSuchProcess, psutil.AccessDenied):
                pass
        time.sleep(0.5)

def sha256(data: str) -> str:
    return hashlib.sha256(data.encode()).hexdigest()

def run_client(args):
    global total_errors, total_success
    user_id, set_ratio, get_ratio = args

    try:
        sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        sock.settimeout(30.0)
        sock.connect((HOST, PORT))

        buffer = ""
        token = sha256(USERNAME + PASSWORD)
        send_batch(sock, [f"Authorization: {USERNAME}.{token}"])
        response, buffer = recv_response(sock, buffer, timeout=10.0)

        if not response or ("OK:" not in response and "authenticated" not in response.lower()):
            sock.close()
            return

        local_errors = 0

        for i in range(REQUESTS_PER_USER):
            key = f"bench_{user_id}_{i}"

            rand = random.random()
            if rand < set_ratio:
                cmd = f"set {key} {json.dumps({'id': i, 'data': random.random()})}"
            elif rand < get_ratio:
                cmd = f"get {key}"
            else:
                cmd = f"del {key}"

            start = time.perf_counter()
            send_command(sock, cmd)

            response, buffer = recv_response(sock, buffer)

            latency = (time.perf_counter() - start) * 1000

            if response and "OK:" in response:
                local_success = 1
            else:
                local_errors += 1
                local_success = 0

            if i % 100 == 0:
                with stats_lock:
                    all_latencies.append(latency)

            with stats_lock:
                total_success += local_success

        with stats_lock:
            total_errors += local_errors

        sock.close()
    except Exception as e:
        with stats_lock:
            total_errors += 1

def run_benchmark(name: str, set_ratio: float, get_ratio: float) -> dict:
    global all_latencies, total_errors, total_success, cpu_samples, mem_samples, stop_monitoring

    all_latencies = []
    total_errors = 0
    total_success = 0
    cpu_samples = []
    mem_samples = []
    stop_monitoring = False

    monitor_thread = Thread(target=monitor_resources, daemon=True)
    monitor_thread.start()

    print(f"\n{'='*60}")
    print(f"  {name}")
    print(f"{'='*60}")
    print(f"  Threads: {CONCURRENT_USERS} | Ops/thread: {REQUESTS_PER_USER}")
    print(f"  Total: {CONCURRENT_USERS * REQUESTS_PER_USER:,} ops")
    print(f"  Mix: SET {int(set_ratio*100)}%, GET {int((get_ratio-set_ratio)*100)}%, DEL {int((1-get_ratio)*100)}%")
    print(f"{'='*60}")

    start_time = time.perf_counter()

    tasks = [(i, set_ratio, get_ratio) for i in range(CONCURRENT_USERS)]
    with ThreadPoolExecutor(max_workers=CONCURRENT_USERS) as executor:
        list(executor.map(run_client, tasks))

    stop_monitoring = True
    monitor_thread.join(timeout=2)

    duration = time.perf_counter() - start_time
    total_ops = total_success

    if all_latencies:
        all_latencies.sort()
        n = len(all_latencies)
        p95_idx = int(n * 0.95)

        throughput = total_ops / duration if duration > 0 else 0

        print(f"\n  Result:")
        print(f"    Duration: {duration:.2f}s")
        print(f"    Success: {total_success:,}")
        print(f"    Throughput: {throughput:.2f} req/s")
        print(f"    Latency min: {min(all_latencies):.3f} ms")
        print(f"    Latency avg: {statistics.mean(all_latencies):.2f} ms")
        print(f"    Latency P95: {all_latencies[p95_idx]:.2f} ms")
        print(f"    Latence max: {max(all_latencies):.2f} ms")
        print(f"    Errors: {total_errors}")
    else:
        print(f"\n  ERROR: No data collected!")
        print(f"  Errors: {total_errors}")

    with monitor_lock:
        if cpu_samples:
            print(f"\n  Resource Usage:")
            print(f"    CPU: {statistics.mean(cpu_samples):.1f}% avg, {max(cpu_samples):.1f}% max")
            print(f"    Memory: {statistics.mean(mem_samples):.1f} MB avg, {max(mem_samples):.1f} MB max")
        cpu_samples = []
        mem_samples = []

    return {"name": name, "duration": duration, "success": total_success, "errors": total_errors, "throughput": throughput if total_ops else 0}

def run_all_benchmarks():
    run_benchmark("Basic Stress", 0.4, 0.8)
    run_benchmark("Read-Heavy", 0.1, 0.8)
    run_benchmark("Write-Heavy", 0.7, 0.9)

def main():
    print("\n" + "="*60)
    print("  ZESTDB BENCHMARK SUITE")
    print("="*60)

    run_all_benchmarks()

    print("\n" + "="*60)
    print("  BENCHMARKS COMPLETED")
    print("="*60)

if __name__ == "__main__":
    main()