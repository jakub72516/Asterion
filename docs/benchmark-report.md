# Asterion Benchmark Report

## 1. Environment

| item | value |
|------|-------|
| OS | Windows 10 x64 (build 19045) |
| CPU | Intel Core i7-11800H @ 2.30 GHz, 8 cores / 16 threads |
| RAM | 16 GiB |
| Compiler | MSVC 19.44 (Visual Studio Build Tools 2022) |
| Build mode | Release (`/O2`, C++20) |
| Generator | CMake 4.2.0, Visual Studio 17 2022 |

## 2. What is being measured

The workload generator (`asterion-bench.exe`) runs a mixed workload against a
**single in-process `Store`** (the storage engine). It models a large logical
keyspace (default `100,000,000` logical keys) while only ever touching a
bounded **working set**; this demonstrates that resident memory is bounded by
configuration and does **not** grow with the logical key count or total stored
data. It is **not** a claim about physical storage capacity.

## 3. Reproducible commands

```powershell
scripts\build.ps1
build\bin\Release\asterion-bench.exe `
  --ops 20000 --working-set 1000 --logical-keys 100000000 `
  --reads 0.7 --skew 1.1 --flush none --seed 7

build\bin\Release\asterion-bench.exe `
  --ops 5000 --working-set 1000 --logical-keys 100000000 `
  --reads 0.5 --skew 1.1 --flush per_write --seed 7
```

The workload mixes writes, reads, scans, deletions, and idempotent retries.

## 4. Results

### 4.1 Mixed workload, warm working set, `flush=none`

```
logical_keys         100000000
working_set          1000
ops                  20000
throughput           846 ops/s
latency mean         1181 us
latency p50          50 us
latency p95          10 ms
latency p99          25 ms
memory (working set) ~3.7 MB
duplicates detected  205 / 205 retries
```

### 4.2 Durable writes, `flush=per_write`

```
ops                  5000
throughput           802 ops/s
latency mean         1245 us
latency p50          2.5 ms
latency p95          5 ms
latency p99          10 ms
memory (working set) ~3.5 MB
```

## 5. Interpretation and honest limitations

- **Memory stays bounded** (~3.5-4 MB working set) regardless of the 100M
  logical keys and the number of operations, because the index is an LRU capped
  at `index_max_entries`.
- **`flush=per_write` is the durability cost.** Each acknowledged write calls
  `FlushFileBuffers`; throughput is therefore bounded by the storage device's
  flush latency, which is the correct price for durable acknowledgement.
- **Cold keys fall back to a full log scan.** A key not present in the index
  requires a scan to determine its latest sequence (for writes) or value (for
  reads). This is the bounded-memory tradeoff: memory is `O(index_max_entries)`
  but cold-key access is `O(log size)`. The p95/p99 tails above are dominated by
  these scan-based cold-key operations and by scans/retries that re-read the
  log. A larger `index_max_entries` (or keeping the hot set warm) removes this
  tail.
- **Single-writer design.** The store serializes writes, so these are
  single-threaded engine numbers. Multi-node throughput scales by partitioning
  write ownership across nodes (see the replication layer).
- These figures are a **workload-simulation demonstration**, not a benchmark of
  physical storage capacity, and not an apples-to-apples comparison with
  network-attached database benchmarks.

## 6. Failure results

The test suite (35 tests) includes deterministic fault scenarios: corruption
and truncation of segment files are detected and isolated on recovery, a
randomized workload is checked against a reference model across simulated
process restarts, and quorum writes fail honestly when replicas are
unavailable. See `scripts\run-fault.ps1`.
