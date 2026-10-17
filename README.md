# Asterion

A Windows-first, fault-tolerant, append-only event database in C++20. Built with
MSVC (Visual Studio 2022), CMake, Windows IOCP networking, and native Windows
file APIs.

## Build

```powershell
scripts\build.ps1                 # configure + build Release
```

Requires Visual Studio 2022 (or Build Tools) with the C++ workload and CMake
>= 3.24. Output binaries are under `build\bin\Release\`.

Three executables are produced:

- `asterion-node.exe`  — runs one database node (`asterion-node <config>`).
- `asterion-cli.exe`   — sends commands/queries to a node or cluster.
- `asterion-tests.exe` — unit / integration / recovery / fault tests.

## Run a single node

```powershell
build\bin\Release\asterion-node.exe config\node.example.json
```

In another terminal:

```powershell
build\bin\Release\asterion-cli.exe 127.0.0.1 7100 status
build\bin\Release\asterion-cli.exe 127.0.0.1 7100 append 1 orders/42 1 "hello"
build\bin\Release\asterion-cli.exe 127.0.0.1 7100 get 1 orders/42
build\bin\Release\asterion-cli.exe 127.0.0.1 7100 scan 1 0 9999999999999999 100
build\bin\Release\asterion-cli.exe 127.0.0.1 7100 shutdown
```

## Run a local cluster

```powershell
scripts\start-cluster.ps1 -Nodes 3    # generates configs + launches 3 nodes
build\bin\Release\asterion-cli.exe 127.0.0.1 7100 append 1 k 1 v
scripts\kill-node.ps1 -Index 2        # force-kill node 2
scripts\restart-node.ps1 -Index 2     # restart it (catch-up on next write)
scripts\stop-cluster.ps1              # graceful shutdown
scripts\clean-test-data.ps1           # remove all run data
```

## Tests

```powershell
scripts\run-tests.ps1            # build + run fast tests
build\bin\Release\asterion-tests.exe --all      # everything (incl. stress)
build\bin\Release\asterion-tests.exe fault_     # deterministic fault scenarios
```

## Benchmark

```powershell
build\bin\Release\asterion-bench.exe --ops 20000 --working-set 1000 `
  --logical-keys 100000000 --reads 0.7 --skew 1.1 --flush none --seed 7
```

## Documentation

- `docs/protocol.md` — wire protocol.
- `docs/on-disk-format.md` — segment/record format and durability guarantees.
- `docs/architecture.md` — architecture and consistency model (safety
  assumptions stated plainly).
- `docs/benchmark-report.md` — reproducible benchmark and honest interpretation.
- `docs/postmortem.md` — the hardest bug found and how it was fixed.

## Layout

```
src/common    config, logging, clock, bytes, json, crc32, platform (file)
src/storage   segment, record, log, key_index
src/core      store, node, cluster (replication)
src/net       server (IOCP), client, handler
src/protocol  frame/message codec
src/metrics   counters + latency histogram
src/fault     deterministic RNG
tests/        test suite (CTest-compatible)
workload/     benchmark generator
scripts/      PowerShell automation
config/       example configurations
```
