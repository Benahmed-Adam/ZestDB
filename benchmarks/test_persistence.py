import socket
import hashlib
import time
import random
import signal
import sys
import os
from concurrent.futures import ThreadPoolExecutor
from threading import Lock, Thread
from typing import List, Dict

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
    
    token = sha256(USERNAME + PASSWORD)
    sock.sendall(f"Authorization: {USERNAME}.{token}\n".encode('utf-8'))
    sock.recv(1024)
    
    test_keys = [f"persist_{i}" for i in range(1000)]
    test_data = {k: f"value_{k}_{random.randint(0, 1000000)}" for k in test_keys}
    
    for key, value in test_data.items():
        cmd = f"set {key} {value}"
        sock.sendall(f"{cmd}\n".encode('utf-8'))
        sock.recv(1024)
    
    sock.close()
    print(f"    {len(test_data)} clés inserées")
    
    print("\n  Phase 2: Vérification...")
    
    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    sock.settimeout(30.0)
    sock.connect((HOST, PORT))
    
    token = sha256(USERNAME + PASSWORD)
    sock.sendall(f"Authorization: {USERNAME}.{token}\n".encode('utf-8'))
    sock.recv(1024)
    
    errors = 0
    for key, expected_value in test_data.items():
        cmd = f"get {key}"
        sock.sendall(f"{cmd}\n".encode('utf-8'))
        
        response = ""
        while "\n" not in response:
            chunk = sock.recv(4096).decode('utf-8')
            if not chunk:
                break
            response += chunk
        
        if expected_value not in response:
            errors += 1
    
    sock.close()
    
    with results_lock:
        verification_errors += errors
    
    print(f"    Vérifié: {len(test_data) - errors}/{len(test_data)}")
    
    return errors

def verify_after_restart():
    global verification_errors
    
    print("\n  Vérification après redémarrage...")
    
    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    sock.settimeout(30.0)
    sock.connect((HOST, PORT))
    
    token = sha256(USERNAME + PASSWORD)
    sock.sendall(f"Authorization: {USERNAME}.{token}\n".encode('utf-8'))
    sock.recv(1024)
    
    errors = 0
    for key, expected_value in test_data.items():
        cmd = f"get {key}"
        sock.sendall(f"{cmd}\n".encode('utf-8'))
        
        response = ""
        while "\n" not in response:
            chunk = sock.recv(4096).decode('utf-8')
            if not chunk:
                break
            response += chunk
        
        if expected_value not in response:
            errors += 1
    
    sock.close()
    
    with results_lock:
        verification_errors += errors
    
    print(f"    Clés vérifiées: {len(test_data) - errors}/{len(test_data)}")
    print(f"    Erreurs: {errors}")

def test_flush_durability():
    global test_data, verification_errors
    
    print("\n  Test: Flush et durabilité...")
    
    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    sock.settimeout(30.0)
    sock.connect((HOST, PORT))
    
    token = sha256(USERNAME + PASSWORD)
    sock.sendall(f"Authorization: {USERNAME}.{token}\n".encode('utf-8'))
    sock.recv(1024)
    
    test_keys = [f"flush_{i}" for i in range(500)]
    test_data = {k: f"flush_value_{k}" for k in test_keys}
    
    for key, value in test_data.items():
        cmd = f"set {key} {value}"
        sock.sendall(f"{cmd}\n".encode('utf-8'))
        sock.recv(1024)
    
    print("    Envoi commande FLUSH...")
    sock.sendall(b"flush\n")
    response = ""
    while "\n" not in response:
        chunk = sock.recv(4096).decode('utf-8')
        if not chunk:
            break
        response += chunk
    
    sock.close()
    
    time.sleep(2)
    
    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    sock.settimeout(30.0)
    sock.connect((HOST, PORT))
    
    token = sha256(USERNAME + PASSWORD)
    sock.sendall(f"Authorization: {USERNAME}.{token}\n".encode('utf-8'))
    sock.recv(1024)
    
    errors = 0
    for key, expected_value in test_data.items():
        cmd = f"get {key}"
        sock.sendall(f"{cmd}\n".encode('utf-8'))
        
        response = ""
        while "\n" not in response:
            chunk = sock.recv(4096).decode('utf-8')
            if not chunk:
                break
            response += chunk
        
        if expected_value not in response:
            errors += 1
    
    sock.close()
    
    print(f"    Clés préservées: {len(test_data) - errors}/{len(test_data)}")
    print(f"    Erreurs: {errors}")

def test_tombstone_persistence():
    print("\n  Test: Tombstones et persistence...")
    
    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    sock.settimeout(30.0)
    sock.connect((HOST, PORT))
    
    token = sha256(USERNAME + PASSWORD)
    sock.sendall(f"Authorization: {USERNAME}.{token}\n".encode('utf-8'))
    sock.recv(1024)
    
    test_keys = [f"tombstone_{i}" for i in range(100)]
    for key in test_keys:
        cmd = f"set {key} tombstone_value"
        sock.sendall(f"{cmd}\n".encode('utf-8'))
        sock.recv(1024)
    
    for key in test_keys[:50]:
        cmd = f"del {key}"
        sock.sendall(f"{cmd}\n".encode('utf-8'))
        sock.recv(1024)
    
    sock.sendall(b"flush\n")
    sock.recv(1024)
    
    sock.close()
    time.sleep(1)
    
    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    sock.settimeout(30.0)
    sock.connect((HOST, PORT))
    
    token = sha256(USERNAME + PASSWORD)
    sock.sendall(f"Authorization: {USERNAME}.{token}\n".encode('utf-8'))
    sock.recv(1024)
    
    deleted_count = 0
    existing_count = 0
    
    for key in test_keys:
        cmd = f"get {key}"
        sock.sendall(f"{cmd}\n".encode('utf-8'))
        
        response = ""
        while "\n" not in response:
            chunk = sock.recv(4096).decode('utf-8')
            if not chunk:
                break
            response += chunk
        
        if "null" in response.lower() or "not found" in response.lower():
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
    verify_after_restart()
    test_flush_durability()
    test_tombstone_persistence()
    
    print("\n" + "="*60)
    print(f"  Erreurs totales: {verification_errors}")
    print("="*60)

if __name__ == "__main__":
    run_persistence_tests()