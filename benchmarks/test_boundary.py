import socket
import hashlib
import time
import random
import string
from concurrent.futures import ThreadPoolExecutor, as_completed
from threading import Lock
from typing import List, Dict

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
    return ''.join(random.choices(string.ascii_letters + string.digits + '   ', k=size))

def test_large_values(user_id: int):
    global error_count
    
    try:
        sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        sock.settimeout(60.0)
        sock.connect((HOST, PORT))
        
        token = sha256(USERNAME + PASSWORD)
        sock.sendall(f"Authorization: {USERNAME}.{token}\n".encode('utf-8'))
        sock.recv(1024)
        
        errors = 0
        sizes = [100, 1000, 10000, 50000, 100000]
        
        for size in sizes:
            key = f"large_{user_id}_{size}"
            value = random_value(size)
            
            cmd = f"set {key} {value}"
            sock.sendall(f"{cmd}\n".encode('utf-8'))
            
            try:
                response = ""
                while "\n" not in response:
                    chunk = sock.recv(4096).decode('utf-8')
                    if not chunk:
                        break
                    response += chunk
            except:
                errors += 1
        
        with results_lock:
            error_count += errors
        
        sock.close()
    except:
        with results_lock:
            error_count += 1

def test_max_values(user_id: int, max_key_size: int, max_value_size: int):
    global error_count
    
    try:
        sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        sock.settimeout(60.0)
        sock.connect((HOST, PORT))
        
        token = sha256(USERNAME + PASSWORD)
        sock.sendall(f"Authorization: {USERNAME}.{token}\n".encode('utf-8'))
        sock.recv(1024)
        
        errors = 0
        
        key = "x" * max_key_size
        value = "y" * max_value_size
        cmd = f"set {key} {value}"
        sock.sendall(f"{cmd}\n".encode('utf-8'))
        
        try:
            response = ""
            while "\n" not in response:
                chunk = sock.recv(4096).decode('utf-8')
                if not chunk:
                    break
                response += chunk
        except:
            errors += 1
        
        cmd = f"get {key}"
        sock.sendall(f"{cmd}\n".encode('utf-8'))
        
        try:
            response = ""
            while "\n" not in response:
                chunk = sock.recv(4096).decode('utf-8')
                if not chunk:
                    break
                response += chunk
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
        
        token = sha256(USERNAME + PASSWORD)
        sock.sendall(f"Authorization: {USERNAME}.{token}\n".encode('utf-8'))
        sock.recv(1024)
        
        errors = 0
        test_cases = [
            "key_with_underscore",
            "key-with-dash",
            "key.with.dots",
            "key:with:colons",
            "key/with/slashes",
            "key\\with\\backslash",
            "key with spaces",
            "key\nwith\nnewlines",
            "key\twith\ttabs",
            "key\x00with\x00null",
            "key\\x00escape",
        ]
        
        for key in test_cases:
            value = f"value_for_{key}"
            cmd = f"set {key} {value}"
            sock.sendall(f"{cmd}\n".encode('utf-8'))
            
            try:
                response = ""
                while "\n" not in response:
                    chunk = sock.recv(4096).decode('utf-8')
                    if not chunk:
                        break
                    response += chunk
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
    with ThreadPoolExecutor(max_workers=CONCURRENT_USERS) as executor:
        list(executor.map(test_max_values, range(CONCURRENT_USERS), [255] * CONCURRENT_USERS, [100000] * CONCURRENT_USERS))
    print(f"    Erreurs: {error_count}")
    
    error_count = 0
    print("\n  Test: Caractères spéciaux...")
    with ThreadPoolExecutor(max_workers=CONCURRENT_USERS) as executor:
        list(executor.map(test_special_characters, range(CONCURRENT_USERS)))
    print(f"    Erreurs: {error_count}")
    
    print("\n" + "="*60)

if __name__ == "__main__":
    run_boundary_tests()