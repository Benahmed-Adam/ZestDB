#!/usr/bin/env python3

import json
import multiprocessing as mp
import sys
import time

from zest_client import ZestClient, base_parser, connect, count_errors, int_list, summarize, write_json


HOT_PREFIX = "bench_conc_hot_"


def command(workload: str, wid: int, i: int, read_pct: int, hot_keys: int) -> str:
    if workload == "incr":
        # Every client increments the same few counters: measures contention on the shard locks
        return f"incr {HOT_PREFIX}{i % hot_keys}"
    k = f"bench_conc_{wid}_{i}"
    return f"g {k}" if i % 100 < read_pct else f's {k} "v{i}"'


def worker(conn_args: tuple, wid: int, ops: int, batch_size: int, read_pct: int, workload: str, hot_keys: int, barrier, results) -> None:
    client = ZestClient(*conn_args)
    if workload == "getset":
        keys = [f"bench_conc_{wid}_{i}" for i in range(ops)]
        for start in range(0, ops, 10_000):
            client.send([f's {k} "v"' for k in keys[start:start + 10_000]])
    barrier.wait()

    latencies, errors, batch = [], 0, []
    t_start = time.perf_counter()
    for i in range(ops):
        batch.append(command(workload, wid, i, read_pct, hot_keys))
        if len(batch) >= batch_size or i == ops - 1:
            ms, reply = client.timed(batch)
            latencies.append(ms)
            errors += count_errors(reply)
            batch = []
    results.put((t_start, time.perf_counter(), latencies, errors))
    client.close()


def hot_total(client) -> int:
    rows = json.loads(client.send([f"gb sw {HOT_PREFIX}"]))["response"]
    return sum(int(value) for row in rows for value in row.values())


def run(args, client, clients: int) -> dict:
    if args.workload == "incr":
        client.send([f"db sw {HOT_PREFIX}"])
    conn_args = (args.host, args.port, args.user, args.password)
    barrier, results = mp.Barrier(clients), mp.Queue()
    procs = [mp.Process(target=worker, args=(conn_args, w, args.ops, args.batch, args.read_pct, args.workload, args.hot_keys, barrier, results)) for w in range(clients)]
    for proc in procs:
        proc.start()
    outs = [results.get() for _ in procs]
    for proc in procs:
        proc.join()

    wall = max(o[1] for o in outs) - min(o[0] for o in outs)
    total_ops = clients * args.ops
    return {
        "clients": clients,
        "ops": total_ops,
        "batch_size": args.batch,
        "workload": args.workload,
        "read_pct": args.read_pct,
        "errors": sum(o[3] for o in outs),
        # incr only: increments that are missing from the counters' final sum, must be 0
        "lost": total_ops - hot_total(client) if args.workload == "incr" else 0,
        "wall_time": wall,
        "throughput": total_ops / wall,
        "per_batch_ms": summarize([ms for o in outs for ms in o[2]]),
    }


def main() -> None:
    p = base_parser(__doc__)
    p.add_argument("--clients", type=int_list, default=[1, 2, 4, 8, 16], help="comma-separated client counts (default: 1,2,4,8,16)")
    p.add_argument("--ops", type=int, default=50_000, help="operations per client (default: 50000)")
    p.add_argument("--batch", type=int, default=100, help="commands per request (default: 100)")
    p.add_argument("--read-pct", type=int, choices=range(0, 101), metavar="0-100", default=80, help="getset workload: percentage of GETs, the rest are SETs (default: 80)")
    p.add_argument("--workload", choices=["getset", "incr"], default="getset", help="getset: GET/SET mix on each client's own keys; incr: every client increments the same counters (default: getset)")
    p.add_argument("--hot-keys", type=int, default=16, help="incr workload: number of shared counters (default: 16)")
    args = p.parse_args()

    client = connect(args)
    results = []
    for n in args.clients:
        sys.stdout.write(f"{n:>3} client(s) ... ")
        sys.stdout.flush()
        results.append(run(args, client, n))
        print(f"{results[-1]['throughput']:,.0f} ops/s")

    client.send(["db sw bench_conc_"])
    client.close()

    title = f"INCR on {args.hot_keys} shared counters" if args.workload == "incr" else f"{args.read_pct}% GET / {100 - args.read_pct}% SET"
    print("\n" + "=" * 95)
    print(f"  {title}, {args.ops:,} ops per client, batch {args.batch}")
    print("-" * 95)
    print(f"  {'Clients':>7} | {'Total ops':>10} | {'Wall (s)':>8} | {'Ops/s':>11} | {'Med batch (ms)':>14} | {'p99 batch (ms)':>14} | {'Errors':>6} | {'Lost':>6}")
    print("-" * 95)
    for r in results:
        s = r["per_batch_ms"]
        print(f"  {r['clients']:>7} | {r['ops']:>10,} | {r['wall_time']:>8.2f} | {r['throughput']:>11,.0f} | {s['med']:>14.3f} | {s['p99']:>14.3f} | {r['errors']:>6,} | {r['lost']:>6,}")
    print("=" * 95)
    write_json(args.json, results)


if __name__ == "__main__":
    main()
