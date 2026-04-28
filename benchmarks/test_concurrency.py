import socket
import hashlib
import time
import statistics
import random
import psutil
from concurrent.futures import ThreadPoolExecutor
from threading import Lock, Barrier, Thread
from typing import List, Dict

HOST = "localhost"
PORT = 7321
USERNAME = "bob"
PASSWORD = "bob"

results_lock = Lock()
all_latencies: List[float] = []
error_count = 0
success_count = 0

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

def monitor_resources():
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

def measure(sock, cmd: str) -> float:
    start = time.perf_counter()
    sock.sendall(f"{cmd}\n".encode('utf-8'))
    
    response = ""
    while "\n" not in response:
        chunk = sock.recv(4096).decode('utf-8')
        if not chunk:
            break
        response += chunk
    
    return (time.perf_counter() - start) * 1000

def burst_worker(worker_id: int, barrier: Barrier):
    global error_count, success_count
    
    try:
        sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        sock.settimeout(30.0)
        sock.connect((HOST, PORT))
        
        token = sha256(USERNAME + PASSWORD)
        sock.sendall(f"Authorization: {USERNAME}.{token}\n".encode('utf-8'))
        sock.recv(1024)
        
        barrier.wait()
        
        local_errors = 0
        local_success = 0
        
        for i in range(100):
            key = f"burst_{worker_id}_{i}"
            cmd = f"set {key} value_{i}"
            
            try:
                lat = measure(sock, cmd)
                
                with results_lock:
                    all_latencies.append(lat)
                    success_count += 1
                    local_success += 1
            except Exception as e:
                with results_lock:
                    error_count += 1
                    local_errors += 1
        
        sock.close()
    except Exception as e:
        with results_lock:
            error_count += 1

def test_burst_concurrency(num_workers: int = 100):
    global all_latencies, error_count, success_count, cpu_samples, mem_samples, stop_monitoring
    
    all_latencies = []
    error_count = 0
    success_count = 0
    cpu_samples = []
    mem_samples = []
    stop_monitoring = False
    
    monitor_thread = Thread(target=monitor_resources, daemon=True)
    monitor_thread.start()
    
    print(f"\n  Burst Concurrency Test ({num_workers} workers)...")
    
    barrier = Barrier(num_workers)
    start = time.perf_counter()
    
    with ThreadPoolExecutor(max_workers=num_workers) as executor:
        for i in range(num_workers):
            executor.submit(burst_worker, i, barrier)
    
    stop_monitoring = True
    monitor_thread.join(timeout=2)
    
    duration = time.perf_counter() - start
    
    if all_latencies:
        all_latencies.sort()
        n = len(all_latencies)
        
        print(f"    Durée: {duration:.2f}s")
        print(f"    Débit: {success_count / duration:.2f} req/s")
        print(f"    Latence min: {min(all_latencies):.3f} ms")
        print(f"    Latence moy: {statistics.mean(all_latencies):.2f} ms")
        if n > 20:
            print(f"    Latence P95: {statistics.quantiles(all_latencies, n=20)[18]:.2f} ms")
        print(f"    Latence max: {max(all_latencies):.2f} ms")
        print(f"    Erreurs: {error_count}")
    
    with monitor_lock:
        if cpu_samples:
            print(f"    Resource Usage:")
            print(f"      CPU: {statistics.mean(cpu_samples):.1f}% avg, {max(cpu_samples):.1f}% max")
            print(f"      Memory: {statistics.mean(mem_samples):.1f} MB avg, {max(mem_samples):.1f} MB max")
    
    return duration, success_count, error_count

def long_running_worker(worker_id: int, duration_sec: int):
    global error_count, success_count
    
    end_time = time.time() + duration_sec
    
    try:
        sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        sock.settimeout(60.0)
        sock.connect((HOST, PORT))
        
        token = sha256(USERNAME + PASSWORD)
        sock.sendall(f"Authorization: {USERNAME}.{token}\n".encode('utf-8'))
        sock.recv(1024)
        
        local_errors = 0
        local_success = 0
        
        while time.time() < end_time:
            key = f"long_{worker_id}_{random.randint(0, 10000)}"
            rand = random.random()
            
            if rand < 0.5:
                cmd = f"set {key} value_{random.randint(0, 1000)}"
            elif rand < 0.8:
                cmd = f"get {key}"
            else:
                cmd = f"del {key}"
            
            try:
                lat = measure(sock, cmd)
                
                with results_lock:
                    all_latencies.append(lat)
                    success_count += 1
                    local_success += 1
            except Exception as e:
                with results_lock:
                    error_count += 1
                    local_errors += 1
        
        sock.close()
    except Exception as e:
        with results_lock:
            error_count += 1

def test_long_running(num_workers: int = 10, duration_sec: int = 30):
    global all_latencies, error_count, success_count, cpu_samples, mem_samples, stop_monitoring
    
    all_latencies = []
    error_count = 0
    success_count = 0
    cpu_samples = []
    mem_samples = []
    stop_monitoring = False
    
    monitor_thread = Thread(target=monitor_resources, daemon=True)
    monitor_thread.start()
    
    print(f"\n  Long Running Test ({num_workers} workers, {duration_sec}s)...")
    
    start = time.perf_counter()
    
    with ThreadPoolExecutor(max_workers=num_workers) as executor:
        for i in range(num_workers):
            executor.submit(long_running_worker, i, duration_sec)
    
    stop_monitoring = True
    monitor_thread.join(timeout=2)
    
    duration = time.perf_counter() - start
    
    if all_latencies:
        all_latencies.sort()
        n = len(all_latencies)
        
        print(f"    Durée réelle: {duration:.2f}s")
        print(f"    Total ops: {success_count}")
        print(f"    Débit: {success_count / duration:.2f} req/s")
        print(f"    Latence min: {min(all_latencies):.3f} ms")
        print(f"    Latence moy: {statistics.mean(all_latencies):.2f} ms")
        if n > 20:
            print(f"    Latence P95: {statistics.quantiles(all_latencies, n=20)[18]:.2f} ms")
        print(f"    Latence max: {max(all_latencies):.2f} ms")
        print(f"    Erreurs: {error_count}")
    
    with monitor_lock:
        if cpu_samples:
            print(f"    Resource Usage:")
            print(f"      CPU: {statistics.mean(cpu_samples):.1f}% avg, {max(cpu_samples):.1f}% max")
            print(f"      Memory: {statistics.mean(mem_samples):.1f} MB avg, {max(mem_samples):.1f} MB max")
    
    return duration, success_count, error_count

def run_concurrency_tests():
    print("\n" + "="*60)
    print("  CONCURRENCY STRESS TESTS")
    print("="*60)
    
    test_burst_concurrency(50)
    test_burst_concurrency(100)
    test_burst_concurrency(200)
    
    test_long_running(10, 30)
    test_long_running(20, 30)
    
    print("\n" + "="*60)

if __name__ == "__main__":
    run_concurrency_tests()