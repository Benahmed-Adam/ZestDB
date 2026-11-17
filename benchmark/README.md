# ZestDB Benchmarks

Python scripts (3.10+, standard library only) that measure a running ZestDB server over the TCP protocol.

- [Scripts](#scripts)
- [Running a full benchmark](#running-a-full-benchmark)
- [Results](#results)
  - [Single-key commands](#single-key-commands-bench_kvpy)
  - [Concurrent clients](#concurrent-clients-bench_concurrencypy)
  - [Pattern and archive commands](#pattern-and-archive-commands-bench_bulkpy)
- [Takeaways](#takeaways)

## Scripts

| Script | Measures |
| --- | --- |
| `bench_kv.py` | Throughput and latency (avg, median, p95, p99) per batch and per command of every single-key command, for several volumes and batch sizes. |
| `bench_bulk.py` | Latency of `count`, `keys`, `getby`, `setby` and `delby` for each mode (`sw`, `ct`, `ew`, `re`) and with `lim`, over a populated database. With `--archive`, it also measures `save`, `lsaves` and `restore`. |
| `bench_concurrency.py` | Aggregate throughput with 1 to N clients in parallel (one process each). The workload is either a GET/SET mix (`--workload getset`) or every client incrementing the same counters (`--workload incr`). |
| `zest_client.py` | Shared client: framing, authentication, statistics. |

`bench_kv.py` runs these modes in order, each on the state the previous ones left. Pass `--modes set,get,incr-new` to run only some of them.

| Mode | Command | Case | Expected code |
| --- | --- | --- | ---: |
| `set` | `s k "v"` | new key | `0` |
| `get` / `get-miss` | `g k` | key present / absent | `0` / `200` |
| `get-hot` | `g k` | as many reads as `get`, cycling over 1% of the keys (the case a cache is for) | `0` |
| `exists` / `exists-miss` | `exists k` | key present / absent | `0` |
| `setnx-new` / `setnx-existing` | `setnx k "v"` | key absent / present | `0` / `207` |
| `cas-match` / `cas-mismatch` | `cas k "v" "w"` | value equal / different | `0` / `208` |
| `incr-new` / `incr-existing` / `decr` | `incr k`, `incr k 5`, `decr k` | key absent / present | `0` |
| `setex` / `ttl` / `expire` / `persist` | TTL commands | key present | `0` |
| `del` / `del-miss` | `d k` | key present / absent | `0` / `200` |

A response counts as an error only when its code differs from the expected one, so the refusals above are not errors. With `--workload incr`, `bench_concurrency.py` also checks that no increment was lost: the `lost` column must stay at `0`.

Every script accepts these options:

- `--host` and `--port`;
- `--user` and `--password` (default `bob` / `bob`);
- `--ssl`, to connect over TLS to a server started with `useSSL: true`. The certificate is not verified, so a self-signed `cert.pem` works as is;
- `--json <file>`, to save the results for comparison.

The scripts write keys under these prefixes:

| Prefix | Script | Cleanup |
| --- | --- | --- |
| `tk_` | `bench_kv.py` | removed with `--cleanup` |
| `bench_user:` and `bench_order:` | `bench_bulk.py` | removed at the end |
| `bench_conc_` | `bench_concurrency.py` | removed at the end |

Run the scripts on a test database: the default `bench_kv.py` run writes 1 million keys, and `bench_bulk.py --archive` replaces the database with the archive it just took.

## Running a full benchmark

Build in Release, then start a fresh server in another terminal. Make sure no other `zestdb` process is running.

```bash
cmake -S . -B build/Release -DCMAKE_BUILD_TYPE=Release && cmake --build build/Release -j
```
```bash
./bin/zestdb
```

Then run the scripts from the repository root. `bench_bulk.py --archive` goes last.

```bash
python3 benchmark/bench_kv.py --batches 100,1000,10000,100000,250000 --cleanup --json benchmark/results/bench_kv.json
```
```bash
python3 benchmark/bench_concurrency.py --json benchmark/results/conc_read80.json
```
```bash
python3 benchmark/bench_concurrency.py --read-pct 20 --json benchmark/results/conc_read20.json
```
```bash
python3 benchmark/bench_concurrency.py --workload incr --json benchmark/results/conc_incr.json
```
```bash
python3 benchmark/bench_bulk.py --entries 100000 --archive --json benchmark/results/bulk.json
```

The per-command statistics are visible on `http://localhost:1237` while the scripts run.

## Results

The raw results are in [`results/`](results/).

**Setup**

- Client and server on the same machine: WSL2 Ubuntu, localhost.
- Server config: `CacheSize: 2000`, `FlushInterval: 10`, `fsyncOnWrite: false`, automatic archiving off.

**Outcome**: 0 errors across every run, and 0 lost increments.

Each benchmark ran once. Differences under ~20% are within run-to-run noise. Numbers depend on the hardware, so use them to compare runs, not as absolute figures.

### Single-key commands (`bench_kv.py`)

Throughput in ops/s, by batch size (commands per request).

**100,000 keys**

| Mode | 100 | 1,000 | 10,000 | 100,000 |
| --- | ---: | ---: | ---: | ---: |
| SET | 155k ¹ | 701k | 743k | 752k |
| GET | 224k | 440k | 467k | 361k |
| GET-HOT | 440k | 927k | 1,065k | 993k |
| GET-MISS | 427k | 750k | 828k | 708k |
| EXISTS | 536k | 869k | 924k | 850k |
| EXISTS-MISS | 518k | 869k | 942k | 928k |
| SETNX-NEW | 269k | 518k | 563k | 541k |
| SETNX-EXISTING | 509k | 789k | 862k | 813k |
| CAS-MATCH | 270k | 388k | 416k | 420k |
| CAS-MISMATCH | 355k | 557k | 607k | 626k |
| INCR-NEW | 337k | 546k | 580k | 569k |
| INCR-EXISTING | 225k | 314k | 295k | 324k |
| DECR | 231k | 316k | 321k | 315k |
| SETEX | 299k | 526k | 518k | 527k |
| TTL | 551k | 885k | 867k | 913k |
| EXPIRE | 331k | 515k | 347k ¹ | 610k |
| PERSIST | 411k | 730k | 796k | 818k |
| DEL | 408k | 673k | 756k | 791k |
| DEL-MISS | 546k | 902k | 940k | 936k |

¹ Outlier: the first run, on a cold server, and a noisy measurement.

**1,000,000 keys**

| Mode | 100 | 1,000 | 10,000 | 100,000 | 250,000 |
| --- | ---: | ---: | ---: | ---: | ---: |
| SET | 424k | 737k | 757k | 646k | 364k |
| GET | 272k | 401k | 369k | 354k | 399k |
| GET-HOT | 414k | 937k | 891k | 839k | 965k |
| GET-MISS | 513k | 807k | 769k | 671k | 834k |
| EXISTS | 474k | 871k | 884k | 807k | 902k |
| EXISTS-MISS | 475k | 879k | 864k | 769k | 918k |
| SETNX-NEW | 287k | 510k | 446k | 486k | 559k |
| SETNX-EXISTING | 541k | 823k | 768k | 773k | 753k |
| CAS-MATCH | 209k | 266k | 259k | 272k | 306k |
| CAS-MISMATCH | 295k | 391k | 382k | 405k | 436k |
| INCR-NEW | 305k | 494k | 442k | 531k | 579k |
| INCR-EXISTING | 182k | 252k | 268k | 292k | 268k |
| DECR | 191k | 267k | 253k | 303k | 247k |
| SETEX | 326k | 365k | 398k | 454k | 378k |
| TTL | 504k | 691k | 723k | 780k | 760k |
| EXPIRE | 382k | 457k | 472k | 588k | 426k |
| PERSIST | 488k | 599k | 650k | 732k | 628k |
| DEL | 367k | 587k | 612k | 700k | 626k |
| DEL-MISS | 600k | 783k | 845k | 808k | 866k |

**p99 latency per batch (ms), 1,000,000 keys**

| Mode | 100 | 1,000 | 10,000 | 100,000 | 250,000 |
| --- | ---: | ---: | ---: | ---: | ---: |
| SET | 0.6 | 1.4 | 12.6 | 174.9 | 1,718.1 |
| GET | 0.8 | 2.9 | 30.9 | 278.5 | 597.7 |
| GET-HOT | 0.5 | 2.3 | 31.6 | 106.0 | 220.3 |
| EXISTS | 0.5 | 1.2 | 10.6 | 106.5 | 225.9 |
| CAS-MATCH | 0.9 | 7.1 | 59.1 | 389.1 | 771.8 |
| INCR-EXISTING | 1.5 | 5.7 | 73.3 | 342.3 | 1,028.7 |
| DEL | 0.7 | 2.5 | 18.7 | 147.1 | 373.2 |

### Concurrent clients (`bench_concurrency.py`)

50,000 operations per client, batch 100. Throughput in ops/s, with the p99 latency per batch in parentheses.

| Clients | 80% GET / 20% SET | 20% GET / 80% SET | `incr` on 16 shared counters |
| ---: | ---: | ---: | ---: |
| 1 | 128k (2.2 ms) | 211k (1.0 ms) | 149k (1.6 ms) |
| 2 | 444k (0.8 ms) | 524k (0.9 ms) | 364k (1.5 ms) |
| 4 | 511k (1.3 ms) | 757k (0.8 ms) | 352k (2.0 ms) |
| 8 | 528k (2.6 ms) | 685k (1.9 ms) | 366k (3.8 ms) |
| 16 | 601k (4.6 ms) | 702k (4.3 ms) | 398k (6.9 ms) |

All three workloads finished with 0 errors and 0 lost increments.

### Pattern and archive commands (`bench_bulk.py`)

Run on 100,000 keys. Each scenario runs 5 times, except `delby`, `save` and `restore`, which run once.

| Scenario | Command | Rows | Median (ms) | p95 (ms) |
| --- | --- | ---: | ---: | ---: |
| count all | `count` | 100,000 | 1.05 | 1.17 |
| count prefix | `count sw bench_user:` | 50,000 | 1.15 | 3.53 |
| count regex | `count re bench_user:\d*8` | 10,000 | 11.52 | 12.08 |
| count no match | `count sw nothing:` | 0 | 0.78 | 1.19 |
| keys prefix, lim 100 | `keys sw bench_user: lim 100` | 100 | 1.80 | 1.83 |
| keys all, lim 1000 | `keys lim 1000` | 1,000 | 4.68 | 5.30 |
| keys prefix, all | `keys sw bench_user:` | 50,000 | 21.59 | 26.24 |
| keys regex | `keys re bench_user:\d*8` | 10,000 | 11.84 | 14.28 |
| getby prefix, lim 100 | `gb sw bench_user: lim 100` | 100 | 0.53 | 0.70 |
| getby prefix, all | `gb sw bench_user:` | 50,000 | 19.62 | 23.58 |
| getby contains | `gb ct :12` | 1,111 | 1.33 | 1.51 |
| getby suffix | `gb ew 99` | 1,000 | 1.37 | 1.42 |
| getby regex | `gb re bench_user:\d*8` | 10,000 | 12.48 | 17.52 |
| getby regex, no match | `gb re (a+)+$` | 0 | 2.14 | 2.41 |
| setby prefix, lim 1000 | `sb sw bench_order: "updated" lim 1000` | 1,000 | 4.62 | 5.48 |
| setby prefix, all | `sb sw bench_order: "updated"` | 50,000 | 5.43 | 6.53 |
| delby prefix, lim 1000 | `db sw bench_order: lim 1000` | 1,000 | 6.01 | — |
| delby prefix, all | `db sw bench_` | 99,000 | 131.37 | — |
| save | `save` | — | 563.77 | — |
| lsaves | `lsaves` | 1 | 0.15 | 0.21 |
| restore | `restore <archive>` | — | 330.11 | — |

## Takeaways

**Single-key commands**

- **Use batches of 1,000 to 10,000 commands.**
  - Going from 100 to 1,000 nearly doubles throughput, which then levels off around 10,000.
  - Beyond that, throughput stops growing while latency climbs fast: the p99 of a SET batch goes from 13 ms at 10,000 to 1.7 s at 250,000.
- **The protocol ceiling is ~900k ops/s per connection.** Commands that barely touch storage (`exists`, `ttl`, missing keys) reach it.
- **The cache doubles read throughput.** GET-HOT runs at ~2× GET.
- **A cold read pays for the disk.** GET-MISS, which only looks up the in-memory index, is faster than GET.
- **Read-modify-write commands are the slowest**, at ~250-420k ops/s. `incr` on an existing key, `decr` and a successful `cas` each pay a cold read plus a write under the write lock. `incr` on a new key, which needs no read, runs at ~500-580k ops/s.
- **Volume**: from 100,000 to 1 million keys, most commands lose 0-15%. CAS and the TTL commands lose 15-35%, because they read data that no longer fits in the cache.

**Concurrent clients**

- **Throughput levels off from 4 clients.** Beyond that, latency grows with the queue.
- **Write-heavy mixes are faster than read-heavy ones**, because writes are grouped in the WAL while cold reads hit the disk.
- **`incr` on shared counters caps at ~370k ops/s from 2 clients.** Every atomic update of a key takes its shard's exclusive lock, so the 16 counters are updated at most 16 at a time. That is the cost of losing no increment.
- The Python clients run on the same cores as the server, so these figures measure the whole setup, not the server alone.

**Pattern commands**

- **Matching is done in memory.** Counting 100,000 keys takes ~1 ms.
- **For `keys` and `getby`, the cost grows with the number of rows returned**: ~20 ms for 50,000 rows, against ~1 ms for `count` on the same set.
- **A regex costs about 10× a prefix** (`std::regex`). The catastrophic-backtracking pattern `(a+)+$` costs no more than a plain scan.
- **`keys … lim N` is slower than `getby … lim N`**, because `keys` returns the first N keys in sorted order. Each shard copies every matching key so the N smallest can be picked.
- **The `setby prefix, all` figure (5.4 ms for 50,000 rows) looks too low** next to `setby … lim 1000`. Treat it with caution until it is checked.
- **Deletion** costs ~1.3 µs per key: 131 ms for 99,000 keys.
- **Archives**: a `save` of 100,000 keys takes ~0.56 s and a `restore` ~0.33 s.
