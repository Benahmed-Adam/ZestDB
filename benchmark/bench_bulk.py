#!/usr/bin/env python3

import json
import sys

from zest_client import connect, base_parser, summarize, write_json

POPULATE_BATCH = 10_000


def populate(client, num: int) -> None:
    batch: list[str] = []
    for i in range(num):
        prefix = "bench_user:" if i % 2 == 0 else "bench_order:"
        batch.append(f's {prefix}{i} "v_{i}"')
        if len(batch) >= POPULATE_BATCH or i == num - 1:
            client.send(batch)
            batch = []


def scenarios() -> list[tuple[str, str]]:
    return [
        ("count all", "count"),
        ("count prefix", "count sw bench_user:"),
        ("count regex", r"count re bench_user:\d*8"),
        ("count no match", "count sw nothing:"),
        ("keys prefix, lim 100", "keys sw bench_user: lim 100"),
        ("keys all, lim 1000", "keys lim 1000"),
        ("keys prefix, all", "keys sw bench_user:"),
        ("keys regex", r"keys re bench_user:\d*8"),
        ("getby prefix, lim 100", "gb sw bench_user: lim 100"),
        ("getby prefix, all", "gb sw bench_user:"),
        ("getby contains", "gb ct :12"),
        ("getby suffix", "gb ew 99"),
        ("getby regex", r"gb re bench_user:\d*8"),
        ("getby regex, no match", "gb re (a+)+$"),
        ("setby prefix, lim 1000", 'sb sw bench_order: "updated" lim 1000'),
        ("setby prefix, all", 'sb sw bench_order: "updated"'),
        ("delby prefix, lim 1000", "db sw bench_order: lim 1000"),
        ("delby prefix, all", "db sw bench_"),
    ]


def measure(client, name: str, cmd: str, runs: int) -> dict:
    latencies, resp = [], {}
    for _ in range(runs):
        ms, reply = client.timed([cmd])
        resp = json.loads(reply)
        if resp["code"] != 0:
            sys.exit(f"{cmd!r} failed with code {resp['code']} (see returncodes.md)")
        latencies.append(ms)
    result = {"scenario": name, "command": cmd, "rows": resp["affectedRows"], "latency_ms": summarize(latencies), "response": resp["response"]}
    print(f"  {name:<26} {result['rows']:>8,} rows  {result['latency_ms']['med']:>10.2f} ms (median)")
    return result


def main() -> None:
    p = base_parser(__doc__)
    p.add_argument("--entries", type=int, default=100_000, help="keys to populate (default: 100000)")
    p.add_argument("--repeat", type=int, default=5, help="runs per scenario (default: 5); delby, save and restore run once")
    p.add_argument("--archive", action="store_true", help="also measure save, lsaves and restore (restore replaces the database with the archive just taken)")
    args = p.parse_args()

    client = connect(args)
    sys.stdout.write(f"Populating {args.entries:,} keys ... ")
    sys.stdout.flush()
    populate(client, args.entries)
    print("done")

    results = []
    if args.archive:
        results.append(measure(client, "save", "save", 1))
        results.append(measure(client, "lsaves", "lsaves", args.repeat))
        latest = results[-1]["response"][-1]
        results.append(measure(client, "restore", f"restore {latest}", 1))
    for name, cmd in scenarios():
        results.append(measure(client, name, cmd, 1 if cmd.startswith("db") else args.repeat))
    for r in results:
        del r["response"]

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
