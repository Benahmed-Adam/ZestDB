# Benchmarks

Python scripts (3.10+, standard library only) that measure a running ZestDB server over the TCP protocol.

| Script | Measures |
| --- | --- |
| `bench_kv.py` | Every single-key command, for several volumes and batch sizes |
| `bench_bulk.py` | The pattern commands, and with `--archive` `save` / `lsaves` / `restore` |
| `bench_concurrency.py` | Parallel clients, on a GET/SET mix or on shared counters |
| `zest_client.py` | The shared client: framing, authentication, TLS, statistics |

Start a server on a test database, then, from the repository root:

```bash
python3 benchmark/bench_kv.py --cleanup
```

Every script accepts `--host`, `--port`, `--user`, `--password`, `--ssl` and `--json <file>`. The scripts write many keys, and `bench_bulk.py --archive` replaces the database: never run them against real data.

**[Full guide, results and analysis ->](../docs/benchmarks.md)**
