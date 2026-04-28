#!/usr/bin/env python3
import sys
import os

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from datetime import datetime
import time


def print_header(title):
    print("\n" + "=" * 70)
    print(f"  {title}")
    print("=" * 70)


def print_subheader(title):
    print("\n" + "-" * 50)
    print(f"  {title}")
    print("-" * 50)


def main():
    print_header("ZESTDB BENCHMARK SUITE")
    print(f"  Date: {datetime.now().strftime('%Y-%m-%d %H:%M:%S')}")
    print(f"  Répertoire: {os.path.dirname(os.path.abspath(__file__))}")
    print("=" * 70)

    start_time = time.time()

    print_subheader("1. Basic Stress Tests")
    try:
        from benchmark_base import run_all_benchmarks
        run_all_benchmarks()
    except Exception as e:
        print(f"  Erreur: {e}")

    print_subheader("2. Boundary Tests (Valeurs limites)")
    try:
        from test_boundary import run_boundary_tests
        run_boundary_tests()
    except Exception as e:
        print(f"  Erreur: {e}")

    print_subheader("3. Concurrency Stress Tests")
    try:
        from test_concurrency import run_concurrency_tests
        run_concurrency_tests()
    except Exception as e:
        print(f"  Erreur: {e}")

    print_subheader("4. Persistence & Durability Tests")
    try:
        from test_persistence import run_persistence_tests
        run_persistence_tests()
    except Exception as e:
        print(f"  Erreur: {e}")

    print_subheader("5. Pattern Matching Tests")
    try:
        from test_pattern import run_pattern_tests
        run_pattern_tests()
    except Exception as e:
        print(f"  Erreur: {e}")

    total_duration = time.time() - start_time

    print_header("RÉSUMÉ FINAL")
    print(f"  Durée totale: {total_duration:.2f} secondes ({total_duration/60:.2f} minutes)")
    print("=" * 70)


if __name__ == "__main__":
    main()