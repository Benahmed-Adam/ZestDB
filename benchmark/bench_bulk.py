#!/usr/bin/env python3

import json
import sys

from zest_client import connect, base_parser, summarize, write_json

POPULATE_BATCH = 10_000


def populate(client, num: int) -> None:
    batch: list[str] = []
    for i in range(num):
        prefix = "bench_user:" if i % 2 == 0 else "bench_order:"
        batch.append(f"s {prefix}{i} v_{i}")
        if len(batch) >= POPULATE_BATCH or i == num - 1:
            client.send(batch)
            batch = []


def scenarios() -> list[tuple[str, str]]:
    return [
        ("getby prefix, lim 100", "gb sw bench_user: lim 100"),
        ("getby prefix, all", "gb sw bench_user:"),
        ("getby contains", "gb ct :12"),
        ("getby suffix", "gb ew 99"),
        ("getby regex", r"gb re bench_user:\d*8"),
        ("getby regex, no match", "gb re (a+)+$"),
        ("setby prefix, lim 1000", "sb sw bench_order: updated lim 1000"),
        ("setby prefix, all", "sb sw bench_order: updated"),
        ("delby prefix, lim 1000", "db sw bench_order: lim 1000"),
        ("delby prefix, all", "db sw bench_"),
    ]


def main() -> None:
    p = base_parser(__doc__)
    p.add_argument("--entries", type=int, default=100_000, help="keys to populate (default: 100000)")
    p.add_argument("--repeat", type=int, default=5, help="runs per scenario (default: 5); delby runs once")
    args = p.parse_args()

    client = connect(args)
    sys.stdout.write(f"Populating {args.entries:,} keys ... ")
    sys.stdout.flush()
    populate(client, args.entries)
    print("done")

    results = []
    for name, cmd in scenarios():
        runs = 1 if cmd.startswith("db") else args.repeat
        latencies, rows = [], 0
        for _ in range(runs):
            ms, reply = client.timed([cmd])
            resp = json.loads(reply)
            if resp["code"] != 0:
                sys.exit(f"{cmd!r} failed: {resp['response']}")
            latencies.append(ms)
            rows = resp["affectedRows"]
        results.append({"scenario": name, "command": cmd, "rows": rows, "latency_ms": summarize(latencies)})
        print(f"  {name:<26} {rows:>8,} rows  {results[-1]['latency_ms']['med']:>10.2f} ms (median)")

    client.close()

    print("\n" + "=" * 92)
    print(f"  {'Scenario':<26} | {'Rows':>8} | {'Runs':>4} | {'Avg (ms)':>9} | {'Med (ms)':>9} | {'Best (ms)':>9} | {'Worst (ms)':>10}")
    print("-" * 92)
    for r in results:
        s = r["latency_ms"]
        print(f"  {r['scenario']:<26} | {r['rows']:>8,} | {s['count']:>4} | {s['avg']:>9.2f} | {s['med']:>9.2f} | {s['best']:>9.2f} | {s['worst']:>10.2f}")
    print("=" * 92)
    write_json(args.json, results)


if __name__ == "__main__":
    main()
