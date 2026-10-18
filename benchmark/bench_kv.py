#!/usr/bin/env python3

import sys
import time

from zest_client import connect, base_parser, count_errors, int_list, summarize, write_json


def run(client, mode: str, num: int, batch_size: int, prefix: str) -> dict:
    latencies: list[float] = []
    errors = 0
    batch: list[str] = []
    t_start = time.perf_counter()
    for i in range(num):
        batch.append(f"s {prefix}{i} v_{i}" if mode == "set" else f"g {prefix}{i}")
        if len(batch) >= batch_size or i == num - 1:
            ms, reply = client.timed(batch)
            latencies.append(ms)
            errors += count_errors(reply)
            batch = []
    total = time.perf_counter() - t_start

    stats = summarize(latencies)
    return {
        "mode": mode.upper(),
        "num": num,
        "batch_size": batch_size,
        "requests": stats["count"],
        "errors": errors,
        "total_time": total,
        "throughput": num / total,
        "per_batch_ms": stats,
        "per_cmd_ms": {k: v / batch_size for k, v in stats.items() if k != "count"},
    }


def print_card(r: dict) -> None:
    b, c = r["per_batch_ms"], r["per_cmd_ms"]
    print("=" * 62)
    print(f"  {r['mode']} — {r['num']:,} entries | batch={r['batch_size']:,}")
    print("=" * 62)
    print(f"  Requests      : {r['requests']:>12,}")
    print(f"  Errors        : {r['errors']:>12,}")
    print(f"  Total time    : {r['total_time']:>12.2f} s")
    print(f"  Throughput    : {r['throughput']:>12,.0f} ops/s")
    print("-" * 62)
    print(f"  {'Metric':<16}{'Per batch (ms)':>17}{'Per command (ms)':>17}")
    for name in ("avg", "med", "p95", "p99", "best", "worst"):
        print(f"  {name:<16}{b[name]:>17.3f}{c[name]:>17.4f}")
    print("=" * 62 + "\n")


def main() -> None:
    p = base_parser(__doc__)
    p.add_argument("--entries", type=int_list, default=[100_000, 1_000_000], help="comma-separated volumes (default: 100000,1000000)")
    p.add_argument("--batches", type=int_list, default=[1, 100, 1_000, 10_000, 100_000, 250_000], help="comma-separated batch sizes")
    p.add_argument("--prefix", default="tk_", help="key prefix (default: tk_)")
    p.add_argument("--cleanup", action="store_true", help="delete the benchmark keys at the end (db sw <prefix>)")
    args = p.parse_args()

    client = connect(args)
    results = []
    for num in args.entries:
        for batch_size in args.batches:
            if batch_size > num:
                continue
            for mode in ("set", "get"):
                sys.stdout.write(f"{mode.upper():<4} | entries {num:,} | batch {batch_size:,} ... ")
                sys.stdout.flush()
                r = run(client, mode, num, batch_size, args.prefix)
                print("done")
                print_card(r)
                results.append(r)

    if args.cleanup:
        client.send([f"db sw {args.prefix}"])
    client.close()

    print("=" * 84)
    print(f"  {'Mode':<5}| {'Entries':>10} | {'Batch':>8} | {'Time (s)':>8} | {'Ops/s':>11} | {'Avg/cmd (ms)':>12} | {'Errors':>6}")
    print("-" * 84)
    for r in results:
        print(f"  {r['mode']:<5}| {r['num']:>10,} | {r['batch_size']:>8,} | {r['total_time']:>8.2f} | {r['throughput']:>11,.0f} | {r['per_cmd_ms']['avg']:>12.5f} | {r['errors']:>6,}")
    print("=" * 84)
    write_json(args.json, results)


if __name__ == "__main__":
    main()
