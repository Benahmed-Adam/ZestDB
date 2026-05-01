import socket
import hashlib
import time
import random
import string
import json
import struct
from concurrent.futures import ThreadPoolExecutor, as_completed
from threading import Lock
from typing import List, Dict

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
ITERATIONS = 10000

results_lock = Lock()
error_count = 0

def sha256(data: str) -> str:
    return hashlib.sha256(data.encode()).hexdigest()

def random_key(length: int = 8) -> str:
    return ''.join(random.choices(string.ascii_letters + string.digits, k=length))

def random_value(max_size: int = 1000) -> str:
    size = random.randint(1, max_size)
    return json.dumps({"data": ''.join(random.choices(string.ascii_letters + string.digits + '   ', k=size)), "size": size})

def test_large_values(user_id: int):
    global error_count

    try:
        sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        sock.settimeout(60.0)
        sock.connect((HOST, PORT))

        buffer = ""
        token = sha256(USERNAME + PASSWORD)
        send_batch(sock, [f"Authorization: {USERNAME}.{token}"])
        response, buffer = recv_response(sock, buffer, timeout=10.0)

        if not response or ("OK:" not in response and "authenticated" not in response.lower()):
            sock.close()
            return

        errors = 0
        sizes = [100, 1000, 10000, 50000, 100000]

        for size in sizes:
            key = f"large_{user_id}_{size}"
            value = random_value(size)

            cmd = f"set {key} {value}"
            send_command(sock, cmd)

            try:
                response, buffer = recv_response(sock, buffer)
            except:
                errors += 1

        with results_lock:
            error_count += errors

        sock.close()
    except:
        with results_lock:
            error_count += 1

def test_max_values(args):
    global error_count
    user_id, max_key_size, max_value_size = args

    try:
        sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        sock.settimeout(60.0)
        sock.connect((HOST, PORT))

        buffer = ""
        token = sha256(USERNAME + PASSWORD)
        send_batch(sock, [f"Authorization: {USERNAME}.{token}"])
        response, buffer = recv_response(sock, buffer, timeout=10.0)

        if not response or ("OK:" not in response and "authenticated" not in response.lower()):
            sock.close()
            return

        errors = 0

        key = "x" * max_key_size
        value = "y" * max_value_size
        cmd = f"set {key} {value}"
        send_command(sock, cmd)

        try:
            response, buffer = recv_response(sock, buffer)
        except:
            errors += 1

        cmd = f"get {key}"
        send_command(sock, cmd)

        try:
            response, buffer = recv_response(sock, buffer)
        except:
            errors += 1

        with results_lock:
            error_count += errors

        sock.close()
    except:
        with results_lock:
            error_count += 1

def test_special_characters(user_id: int):
    global error_count

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

        errors = 0
        test_cases = [
            "key_with_underscore",
            "key-with-dash",
            "key.with.dots",
            "key:with:colons",
            "key/with/slashes",
            "key\\with\\backslash",
            "key with spaces",
        ]

        for key in test_cases:
            value = json.dumps({"key": key, "type": "special"})
            cmd = f"set {key} {value}"
            send_command(sock, cmd)

            try:
                response, buffer = recv_response(sock, buffer)
            except:
                errors += 1

        with results_lock:
            error_count += errors

        sock.close()
    except:
        with results_lock:
            error_count += 1

def run_boundary_tests():
    print("\n" + "="*60)
    print("  BOUNDARY TESTS - Valeurs limites")
    print("="*60)

    global error_count
    error_count = 0

    print("\n  Test: Grandes valeurs...")
    with ThreadPoolExecutor(max_workers=CONCURRENT_USERS) as executor:
        list(executor.map(test_large_values, range(CONCURRENT_USERS)))
    print(f"    Erreurs: {error_count}")

    error_count = 0
    print("\n  Test: Valeurs maximales (255/100000)...")
    max_key_size = 255
    max_value_size = 100000
    tasks = [(i, max_key_size, max_value_size) for i in range(CONCURRENT_USERS)]
    with ThreadPoolExecutor(max_workers=CONCURRENT_USERS) as executor:
        list(executor.map(test_max_values, tasks))
    print(f"    Erreurs: {error_count}")

    error_count = 0
    print("\n  Test: Caractères spéciaux...")
    with ThreadPoolExecutor(max_workers=CONCURRENT_USERS) as executor:
        list(executor.map(test_special_characters, range(CONCURRENT_USERS)))
    print(f"    Erreurs: {error_count}")

    print("\n" + "="*60)

if __name__ == "__main__":
    run_boundary_tests()