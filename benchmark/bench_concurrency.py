#!/usr/bin/env python3

import multiprocessing as mp
import sys
import time

from zest_client import ZestClient, base_parser, connect, count_errors, int_list, summarize, write_json


def worker(conn_args: tuple, wid: int, ops: int, batch_size: int, read_pct: int, barrier, results) -> None:
    client = ZestClient(*conn_args)
    keys = [f"bench_conc_{wid}_{i}" for i in range(ops)]
    for start in range(0, ops, 10_000):
        client.send([f"s {k} v" for k in keys[start:start + 10_000]])
    barrier.wait()

    latencies, errors, batch = [], 0, []
    t_start = time.perf_counter()
    for i, k in enumerate(keys):
        batch.append(f"g {k}" if i % 100 < read_pct else f"s {k} v{i}")
        if len(batch) >= batch_size or i == ops - 1:
            ms, reply = client.timed(batch)
            latencies.append(ms)
            errors += count_errors(reply)
            batch = []
    results.put((t_start, time.perf_counter(), latencies, errors))
    client.close()


def run(args, clients: int) -> dict:
    conn_args = (args.host, args.port, args.user, args.password)
    barrier, results = mp.Barrier(clients), mp.Queue()
    procs = [mp.Process(target=worker, args=(conn_args, w, args.ops, args.batch, args.read_pct, barrier, results)) for w in range(clients)]
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
        "read_pct": args.read_pct,
        "errors": sum(o[3] for o in outs),
        "wall_time": wall,
        "throughput": total_ops / wall,
        "per_batch_ms": summarize([ms for o in outs for ms in o[2]]),
    }


def main() -> None:
    p = base_parser(__doc__)
    p.add_argument("--clients", type=int_list, default=[1, 2, 4, 8, 16], help="comma-separated client counts (default: 1,2,4,8,16)")
    p.add_argument("--ops", type=int, default=50_000, help="operations per client (default: 50000)")
    p.add_argument("--batch", type=int, default=100, help="commands per request (default: 100)")
    p.add_argument("--read-pct", type=int, choices=range(0, 101), metavar="0-100", default=80, help="percentage of GETs, the rest are SETs (default: 80)")
    args = p.parse_args()

    client = connect(args)
    results = []
    for n in args.clients:
        sys.stdout.write(f"{n:>3} client(s) ... ")
        sys.stdout.flush()
        results.append(run(args, n))
        print(f"{results[-1]['throughput']:,.0f} ops/s")

    client.send(["db sw bench_conc_"])
    client.close()

    print("\n" + "=" * 86)
    print(f"  {args.read_pct}% GET / {100 - args.read_pct}% SET, {args.ops:,} ops per client, batch {args.batch}")
    print("-" * 86)
    print(f"  {'Clients':>7} | {'Total ops':>10} | {'Wall (s)':>8} | {'Ops/s':>11} | {'Med batch (ms)':>14} | {'p99 batch (ms)':>14} | {'Errors':>6}")
    print("-" * 86)
    for r in results:
        s = r["per_batch_ms"]
        print(f"  {r['clients']:>7} | {r['ops']:>10,} | {r['wall_time']:>8.2f} | {r['throughput']:>11,.0f} | {s['med']:>14.3f} | {s['p99']:>14.3f} | {r['errors']:>6,}")
    print("=" * 86)
    write_json(args.json, results)


if __name__ == "__main__":
    main()
