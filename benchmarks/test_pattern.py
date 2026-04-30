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
            ("getby", "re", "pattern_.*"),
            ("getby", "re", "test_[0-9]+"),
            ("getby", "re", "user_.*"),
            ("getby", "re", "data_.*"),
            ("getby", "re", ".*"),
        ]
        
        for op, mode, pattern in patterns:
            cmd = f"{op} {mode} {pattern}"
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
            ("re", "test_setby_.*"),
            ("re", "user_setby_.*"),
            ("re", "data_setby_.*"),
        ]
        
        for mode, pattern in patterns:
            value = json.dumps({"pattern": pattern, "data": "test"})
            cmd = f"setby {mode} {pattern} {value}"
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
            ("re", "delete_test_.*"),
            ("re", "delete_user_.*"),
        ]
        
        for mode, pattern in patterns:
            cmd = f"delby {mode} {pattern}"
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

def test_sw_mode(user_id: int):
    global error_count
    
    try:
        sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        sock.settimeout(30.0)
        sock.connect((HOST, PORT))
        
        token = sha256(USERNAME + PASSWORD)
        sock.sendall(f"Authorization: {USERNAME}.{token}\n".encode('utf-8'))
        sock.recv(1024)
        
        errors = 0
        
        for prefix in ["user", "data", "test"]:
            cmd = f"getby sw {prefix}"
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

def test_ct_mode(user_id: int):
    global error_count
    
    try:
        sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        sock.settimeout(30.0)
        sock.connect((HOST, PORT))
        
        token = sha256(USERNAME + PASSWORD)
        sock.sendall(f"Authorization: {USERNAME}.{token}\n".encode('utf-8'))
        sock.recv(1024)
        
        errors = 0
        
        for pattern in ["abc", "123", "test"]:
            cmd = f"getby ct {pattern}"
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

def test_ew_mode(user_id: int):
    global error_count
    
    try:
        sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        sock.settimeout(30.0)
        sock.connect((HOST, PORT))
        
        token = sha256(USERNAME + PASSWORD)
        sock.sendall(f"Authorization: {USERNAME}.{token}\n".encode('utf-8'))
        sock.recv(1024)
        
        errors = 0
        
        for suffix in ["_tmp", "_bak", "_log"]:
            cmd = f"getby ew {suffix}"
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

def test_setby_modes(user_id: int):
    global error_count
    
    try:
        sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        sock.settimeout(30.0)
        sock.connect((HOST, PORT))
        
        token = sha256(USERNAME + PASSWORD)
        sock.sendall(f"Authorization: {USERNAME}.{token}\n".encode('utf-8'))
        sock.recv(1024)
        
        errors = 0
        
        cmd = f"setby sw user_ {json.dumps({'mode': 'sw', 'data': 'test'})}"
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
        
        cmd = f"setby ct abc {json.dumps({'mode': 'ct', 'data': 'test'})}"
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
        
        cmd = f"setby ew _tmp {json.dumps({'mode': 'ew', 'data': 'test'})}"
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

def test_delby_modes(user_id: int):
    global error_count
    
    try:
        sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        sock.settimeout(30.0)
        sock.connect((HOST, PORT))
        
        token = sha256(USERNAME + PASSWORD)
        sock.sendall(f"Authorization: {USERNAME}.{token}\n".encode('utf-8'))
        sock.recv(1024)
        
        errors = 0
        
        cmd = f"delby sw delete_"
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
        
        cmd = f"delby ct remove"
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
        
        cmd = f"delby ew _old"
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

def test_limit_option(user_id: int):
    global error_count
    
    try:
        sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        sock.settimeout(30.0)
        sock.connect((HOST, PORT))
        
        token = sha256(USERNAME + PASSWORD)
        sock.sendall(f"Authorization: {USERNAME}.{token}\n".encode('utf-8'))
        sock.recv(1024)
        
        errors = 0
        
        for mode in ["re", "sw", "ct", "ew"]:
            cmd = f"getby {mode} test lim 5"
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

def test_limit_verification():
    global error_count
    
    print("\n    Test: Vérification du paramètre lim...")
    
    try:
        sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        sock.settimeout(30.0)
        sock.connect((HOST, PORT))
        
        token = sha256(USERNAME + PASSWORD)
        sock.sendall(f"Authorization: {USERNAME}.{token}\n".encode('utf-8'))
        sock.recv(1024)
        
        test_prefix = "limtest_"
        num_keys = 20
        for i in range(num_keys):
            key = f"{test_prefix}{i}"
            value = json.dumps({"index": i})
            cmd = f"set {key} {value}"
            sock.sendall(f"{cmd}\n".encode('utf-8'))
            sock.recv(4096)
        
        test_cases = [
            ("sw", test_prefix, 5),
            ("sw", test_prefix, 10),
            ("sw", test_prefix, 0),
            ("sw", test_prefix, 100),
        ]
        
        errors = 0
        for mode, pattern, limit in test_cases:
            cmd = f"getby {mode} {pattern} lim {limit}"
            sock.sendall(f"{cmd}\n".encode('utf-8'))
            
            try:
                response = ""
                start = time.time()
                while time.time() - start < 5:
                    chunk = sock.recv(8192).decode('utf-8')
                    if not chunk:
                        break
                    response += chunk
                    if "\n" in response:
                        break
                
                count = response.count("\n")
                print(f"      {mode} {pattern} lim {limit} -> {count} lignes reçues")
            except Exception as e:
                errors += 1
                print(f"      Erreur pour {cmd}: {e}")
        
        cmd = f"delby sw {test_prefix}"
        sock.sendall(f"{cmd}\n".encode('utf-8'))
        sock.recv(4096)
        
        sock.close()
        
        with results_lock:
            error_count += errors
            
    except Exception as e:
        print(f"    Erreur globale: {e}")
        with results_lock:
            error_count += 1

def run_pattern_tests():
    print("\n" + "="*60)
    print("  PATTERN MATCHING TESTS (GETBY/SETBY/DELBY)")
    print("="*60)
    
    global error_count
    error_count = 0
    
    print("\n  Test: GETBY (re - regex)...")
    with ThreadPoolExecutor(max_workers=10) as executor:
        list(executor.map(test_getby_pattern, range(10)))
    print(f"    Erreurs: {error_count}")
    
    error_count = 0
    print("\n  Test: GETBY (sw - starts with)...")
    with ThreadPoolExecutor(max_workers=10) as executor:
        list(executor.map(test_sw_mode, range(10)))
    print(f"    Erreurs: {error_count}")
    
    error_count = 0
    print("\n  Test: GETBY (ct - contains)...")
    with ThreadPoolExecutor(max_workers=10) as executor:
        list(executor.map(test_ct_mode, range(10)))
    print(f"    Erreurs: {error_count}")
    
    error_count = 0
    print("\n  Test: GETBY (ew - ends with)...")
    with ThreadPoolExecutor(max_workers=10) as executor:
        list(executor.map(test_ew_mode, range(10)))
    print(f"    Erreurs: {error_count}")
    
    error_count = 0
    print("\n  Test: SETBY (re)...")
    with ThreadPoolExecutor(max_workers=10) as executor:
        list(executor.map(test_setby_pattern, range(10)))
    print(f"    Erreurs: {error_count}")
    
    error_count = 0
    print("\n  Test: SETBY (sw/ct/ew)...")
    with ThreadPoolExecutor(max_workers=10) as executor:
        list(executor.map(test_setby_modes, range(10)))
    print(f"    Erreurs: {error_count}")
    
    error_count = 0
    print("\n  Test: DELBY (re)...")
    with ThreadPoolExecutor(max_workers=10) as executor:
        list(executor.map(test_delby_pattern, range(10)))
    print(f"    Erreurs: {error_count}")
    
    error_count = 0
    print("\n  Test: DELBY (sw/ct/ew)...")
    with ThreadPoolExecutor(max_workers=10) as executor:
        list(executor.map(test_delby_modes, range(10)))
    print(f"    Erreurs: {error_count}")
    
    error_count = 0
    print("\n  Test: Limit option (lim)...")
    with ThreadPoolExecutor(max_workers=10) as executor:
        list(executor.map(test_limit_option, range(10)))
    print(f"    Erreurs: {error_count}")
    
    error_count = 0
    print("\n  Test: Limite paramètre vérification...")
    test_limit_verification()
    print(f"    Erreurs: {error_count}")
    
    print("\n" + "="*60)

if __name__ == "__main__":
    run_pattern_tests()