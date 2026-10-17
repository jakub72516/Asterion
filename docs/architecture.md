# Asterion Architecture and Consistency Model

## 1. Overview

Asterion is a Windows-first, append-only event database. A node has three
layers:

```
asterion-node
  └─ Server (IOCP, async networking)
       └─ Node (request handler) ── Cluster (replication)
            └─ Store ── Log (segments) + KeyIndex (bounded LRU)
```

## 2. Storage model

- Events are immutable and append-only.
- A **segment** is a versioned, checksummed file (see `on-disk-format.md`).
- The **index** is a bounded-memory LRU from `(tenant, key)` to the location of
  the latest event. Its size is capped by `storage.index_max_entries`, so
  resident memory is `O(index_max_entries)` and does **not** grow with total
  stored data. A key not present in the index is resolved by a backward scan of
  the log (correct but slower); this is the core bounded-memory tradeoff.

### Ordering, timestamps, deletion, duplicates

- **Per-key ordering** is by `sequence`, a per-key `uint64`. A write with
  `sequence == 0` gets the next number (`latest + 1`) assigned by the node;
  otherwise the client supplies it. A sequence <= the key's latest sequence is a
  retry or a conflict.
- **Timestamps** are wall-clock microseconds (UTC), client- or node-assigned,
  and are metadata for time-range scans only — never the ordering key.
- **Deletion** is a tombstone event (`deleted=1`, empty payload) ordered like
  any other event. A key whose latest event is a tombstone reads as "deleted".
- **Idempotency**: a write is identified by `event_id`. Retrying a write with
  the same `event_id` (and thus the same `sequence`) returns success without
  creating a second event. The exact dedup path: if `sequence <= latest`, the
  node locates the existing record for `(key, sequence)` and compares
  `event_id`; a match is a duplicate, a mismatch is a conflict. This is durable
  and does not require unbounded per-key state (it reads the log on demand).

## 3. Concurrency

- The `Store` serializes writes and scans with a single mutex (single-writer,
  many-reader in spirit; in practice serialized). This keeps per-key ordering
  and idempotency trivially correct.
- The IOCP completion threads only perform socket I/O; request handling
  (including all disk I/O) runs on a separate worker thread pool, so disk
  latency never blocks completion processing.

## 4. Backpressure and limits

Every queue is bounded and overload is a *defined* response, never unbounded
growth:

- `max_connections`, `max_frame_size`, `recv_buffer_size`, `send_queue_max_bytes`,
  `max_queued_requests`.
- A connection whose send queue exceeds the limit is dropped (the client
  retries, idempotently).
- A frame exceeding `max_frame_size` is rejected and the connection closed.
- `scan` results are bounded by `max_results`; scans honor cancellation.

## 5. Replication and consistency model

Asterion uses **primary-copy replication with deterministic partition
assignment**. This is *not* a consensus protocol (it is not Raft or Paxos), and
it does not provide linearizable reads or automatic, split-brain-proof failover.
These properties are stated plainly, not claimed.

### 5.1 Partitioning and replication

- A key's partition is `partition_hash(tenant, key) % partition_count`.
- Nodes are sorted by `node_id`. The replica set for partition `p` is the
  `replication_factor` nodes starting at index `p mod N`. The first is the
  **leader** (primary); the rest are followers.
- Each node stores the union of the partitions it is a replica of (leader or
  follower), so every event has `replication_factor` copies. If fewer than
  `replication_factor` nodes are configured, the effective factor is the node
  count (so a 3-node cluster is RF=3).

### 5.2 Write path

1. A client sends `AppendReq` to any node. If that node is not the leader for
   the key's partition, it returns `not_leader` with the leader id.
2. The leader applies the write to its local log (durably, per flush mode),
   assigns the sequence/timestamp, and sends `ReplicateReq` to each follower.
3. The leader waits for acknowledgements per the consistency level:
   `one` = leader only, `quorum` = majority of the replica set, `all` = all
   replicas. If the required count cannot be reached, the leader returns
   `quorum_unavailable` — it never reports success it did not achieve (the
   local write remains durable; a retry is idempotent).

### 5.3 Failure model and safety assumptions

- The leader is assigned deterministically from configuration; there is **no
  automatic leader election** in this release. If a leader fails, writes to its
  partitions fail with `quorum_unavailable`/connection errors until membership
  is changed and the nodes restarted. This is a documented operational limit.
- Follower catch-up after disconnection is via `SyncReq` (full partition scan
  + idempotent apply).
- Replication queues are bounded by connection limits and the synchronous
  request/response flow; there is no unbounded replication backlog.

### 5.4 What is *not* guaranteed

- No linearizability across partitions; reads on a follower may lag a
  just-acknowledged write.
- No fencing tokens / epochs: a partitioned "old" leader could, in principle,
  accept writes it cannot replicate (it will then fail quorum honestly, but a
  quorum of old followers could also exist). Do not rely on this for
  correctness under arbitrary network partitions.

## 6. Platform abstractions

`common/platform/file.hpp` (storage), `net/socket` + `Server` (IOCP),
`common/clock.hpp` (steady + wall clock), and process control are isolated so a
future non-Windows port changes only these files.

## 7. Lifecycle

`asterion-node <config>` validates its config, creates/verifies its data
directory, opens (recovers) the store, starts the IOCP server and (optionally)
the background compactor, and on `Ctrl+C` or a `ShutdownReq` drains in-flight
work (queued jobs are processed; the log is flushed and closed) before exiting.
Acknowledged data is never silently discarded on graceful shutdown.
