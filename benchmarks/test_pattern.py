import socket
import hashlib
import time
import random
import re
import json
from concurrent.futures import ThreadPoolExecutor
from threading import Lock

HOST = "localhost"
PORT = 7321
USERNAME = "bob"
PASSWORD = "bob"

results_lock = Lock()
error_count = 0

def sha256(data: str) -> str:
    return hashlib.sha256(data.encode()).hexdigest()

def test_getby_pattern(user_id: int):
    global error_count
    
    try:
        sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        sock.settimeout(30.0)
        sock.connect((HOST, PORT))
        
        token = sha256(USERNAME + PASSWORD)
        sock.sendall(f"Authorization: {USERNAME}.{token}\n".encode('utf-8'))
        sock.recv(1024)
        
        errors = 0
        
        patterns = [
            ("getby", "pattern_.*"),
            ("getby", "test_[0-9]+"),
            ("getby", "user_.*"),
            ("getby", "data_.*"),
            ("getby", ".*"),
        ]
        
        for op, pattern in patterns:
            cmd = f"{op} {pattern}"
            sock.sendall(f"{cmd}\n".encode('utf-8'))
            
            try:
                response = ""
                start = time.time()
                while time.time() - start < 5:
                    chunk = sock.recv(4096).decode('utf-8')
                    if not chunk:
                        break
                    response += chunk
                    if "\n" in response:
                        break
            except:
                errors += 1
        
        with results_lock:
            error_count += errors
        
        sock.close()
    except:
        with results_lock:
            error_count += 1

def test_setby_pattern(user_id: int):
    global error_count
    
    try:
        sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        sock.settimeout(30.0)
        sock.connect((HOST, PORT))
        
        token = sha256(USERNAME + PASSWORD)
        sock.sendall(f"Authorization: {USERNAME}.{token}\n".encode('utf-8'))
        sock.recv(1024)
        
        errors = 0
        
        patterns = [
            "test_setby_.*",
            "user_setby_.*",
            "data_setby_.*",
        ]
        
        for pattern in patterns:
            value = json.dumps({"pattern": pattern, "data": "test"})
            cmd = f"setby {pattern} {value}"
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

def test_delby_pattern(user_id: int):
    global error_count
    
    try:
        sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        sock.settimeout(30.0)
        sock.connect((HOST, PORT))
        
        token = sha256(USERNAME + PASSWORD)
        sock.sendall(f"Authorization: {USERNAME}.{token}\n".encode('utf-8'))
        sock.recv(1024)
        
        errors = 0
        
        patterns = [
            "delete_test_.*",
            "delete_user_.*",
        ]
        
        for pattern in patterns:
            cmd = f"delby {pattern}"
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

def run_pattern_tests():
    print("\n" + "="*60)
    print("  PATTERN MATCHING TESTS (GETBY/SETBY/DELBY)")
    print("="*60)
    
    global error_count
    error_count = 0
    
    print("\n  Test: GETBY pattern...")
    with ThreadPoolExecutor(max_workers=10) as executor:
        list(executor.map(test_getby_pattern, range(10)))
    print(f"    Erreurs: {error_count}")
    
    error_count = 0
    print("\n  Test: SETBY pattern...")
    with ThreadPoolExecutor(max_workers=10) as executor:
        list(executor.map(test_setby_pattern, range(10)))
    print(f"    Erreurs: {error_count}")
    
    error_count = 0
    print("\n  Test: DELBY pattern...")
    with ThreadPoolExecutor(max_workers=10) as executor:
        list(executor.map(test_delby_pattern, range(10)))
    print(f"    Erreurs: {error_count}")
    
    print("\n" + "="*60)

if __name__ == "__main__":
    run_pattern_tests()