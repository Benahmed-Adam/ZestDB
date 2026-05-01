import socket
import hashlib
import time
import random
import signal
import sys
import os
import json
import struct
from concurrent.futures import ThreadPoolExecutor
from threading import Lock, Thread
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

results_lock = Lock()
test_data: Dict[str, str] = {}
verification_errors = 0

def sha256(data: str) -> str:
    return hashlib.sha256(data.encode()).hexdigest()

def populate_and_verify():
    global test_data, verification_errors

    print("\n  Phase 1: Population...")

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

    test_keys = [f"persist_{i}" for i in range(1000)]
    test_data = {k: json.dumps({"key": k, "value": random.randint(0, 10000000)}) for k in test_keys}

    batch = []
    for key, value in test_data.items():
        batch.append(f"set {key} {value}")
        if len(batch) == 50:
            send_batch(sock, batch)
            responses, buffer = recv_responses(sock, len(batch), buffer)
            batch = []
    if batch:
        send_batch(sock, batch)
        responses, buffer = recv_responses(sock, len(batch), buffer)

    sock.close()
    print(f"    {len(test_data)} clés inserées")

    print("\n  Phase 2: Vérification...")

    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    sock.settimeout(30.0)
    sock.connect((HOST, PORT))

    buffer = ""
    token = sha256(USERNAME + PASSWORD)
    send_batch(sock, [f"Authorization: {USERNAME}.{token}"])
    response, buffer = recv_response(sock, buffer, timeout=10.0)

    errors = 0
    for key, expected_value in test_data.items():
        cmd = f"get {key}"
        send_command(sock, cmd)
        response, buffer = recv_response(sock, buffer)

        if expected_value not in (response or ""):
            errors += 1

    sock.close()

    with results_lock:
        verification_errors += errors

    print(f"    Vérifié: {len(test_data) - errors}/{len(test_data)}")

    return errors

def verify_data_integrity(test_data: dict, description: str = "Vérification"):
    global verification_errors

    print(f"\n  {description}...")

    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    sock.settimeout(30.0)
    sock.connect((HOST, PORT))

    buffer = ""
    token = sha256(USERNAME + PASSWORD)
    send_batch(sock, [f"Authorization: {USERNAME}.{token}"])
    response, buffer = recv_response(sock, buffer, timeout=10.0)

    errors = 0
    for key, expected_value in test_data.items():
        cmd = f"get {key}"
        send_command(sock, cmd)
        response, buffer = recv_response(sock, buffer)

        if expected_value not in (response or ""):
            errors += 1

    sock.close()

    with results_lock:
        verification_errors += errors

    print(f"    Clés vérifiées: {len(test_data) - errors}/{len(test_data)}")
    print(f"    Erreurs: {errors}")
    return errors

def test_flush_durability():
    global test_data, verification_errors

    print("\n  Test: Flush et durabilité...")

    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    sock.settimeout(30.0)
    sock.connect((HOST, PORT))

    buffer = ""
    token = sha256(USERNAME + PASSWORD)
    send_batch(sock, [f"Authorization: {USERNAME}.{token}"])
    response, buffer = recv_response(sock, buffer, timeout=10.0)

    test_keys = [f"flush_{i}" for i in range(500)]
    test_data = {k: json.dumps({"key": k, "type": "flush"}) for k in test_keys}

    batch = []
    for key, value in test_data.items():
        batch.append(f"set {key} {value}")
        if len(batch) == 50:
            send_batch(sock, batch)
            responses, buffer = recv_responses(sock, len(batch), buffer)
            batch = []
    if batch:
        send_batch(sock, batch)
        responses, buffer = recv_responses(sock, len(batch), buffer)

    print("    Envoi commande FLUSH...")
    send_command(sock, "flush")
    response, buffer = recv_response(sock, buffer)

    sock.close()

    time.sleep(2)

    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    sock.settimeout(30.0)
    sock.connect((HOST, PORT))

    buffer = ""
    token = sha256(USERNAME + PASSWORD)
    send_batch(sock, [f"Authorization: {USERNAME}.{token}"])
    response, buffer = recv_response(sock, buffer, timeout=10.0)

    errors = 0
    for key, expected_value in test_data.items():
        cmd = f"get {key}"
        send_command(sock, cmd)
        response, buffer = recv_response(sock, buffer)

        if expected_value not in (response or ""):
            errors += 1

    sock.close()

    print(f"    Clés préservées: {len(test_data) - errors}/{len(test_data)}")
    print(f"    Erreurs: {errors}")

def test_tombstone_persistence():
    print("\n  Test: Tombstones et persistence...")

    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    sock.settimeout(30.0)
    sock.connect((HOST, PORT))

    buffer = ""
    token = sha256(USERNAME + PASSWORD)
    send_batch(sock, [f"Authorization: {USERNAME}.{token}"])
    response, buffer = recv_response(sock, buffer, timeout=10.0)

    test_keys = [f"tombstone_{i}" for i in range(100)]
    batch = []
    for key in test_keys:
        batch.append(f"set {key} {json.dumps({'key': key, 'deleted': False})}")
        if len(batch) == 50:
            send_batch(sock, batch)
            responses, buffer = recv_responses(sock, len(batch), buffer)
            batch = []
    if batch:
        send_batch(sock, batch)
        responses, buffer = recv_responses(sock, len(batch), buffer)

    batch = []
    for key in test_keys[:50]:
        batch.append(f"del {key}")
        if len(batch) == 50:
            send_batch(sock, batch)
            responses, buffer = recv_responses(sock, len(batch), buffer)
            batch = []
    if batch:
        send_batch(sock, batch)
        responses, buffer = recv_responses(sock, len(batch), buffer)

    send_command(sock, "flush")
    response, buffer = recv_response(sock, buffer)

    sock.close()
    time.sleep(1)

    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    sock.settimeout(30.0)
    sock.connect((HOST, PORT))

    buffer = ""
    token = sha256(USERNAME + PASSWORD)
    send_batch(sock, [f"Authorization: {USERNAME}.{token}"])
    response, buffer = recv_response(sock, buffer, timeout=10.0)

    deleted_count = 0
    existing_count = 0

    for key in test_keys:
        cmd = f"get {key}"
        send_command(sock, cmd)
        response, buffer = recv_response(sock, buffer)

        if not response or "null" in response.lower() or "not found" in response.lower():
            deleted_count += 1
        else:
            existing_count += 1

    sock.close()

    print(f"    Clés supprimées: {deleted_count} (attendu: 50)")
    print(f"    Clés existantes: {existing_count} (attendu: 50)")

def run_persistence_tests():
    print("\n" + "="*60)
    print("  PERSISTANCE & DURABILITÉ TESTS")
    print("="*60)

    populate_and_verify()
    verify_data_integrity(test_data, "Vérification après population")
    test_flush_durability()
    test_tombstone_persistence()

    print("\n" + "="*60)
    print(f"  Erreurs totales: {verification_errors}")
    print("="*60)

if __name__ == "__main__":
    run_persistence_tests()