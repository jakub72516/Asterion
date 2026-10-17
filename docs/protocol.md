# Asterion Network Protocol Specification

Version 1. All integers are little-endian. A peer MUST reject a frame whose
version, magic, size, or checksum are invalid.

## 1. Framing

Every message is a length-delimited frame:

```
offset  size  field
0       4     magic            = 0x41535431 ("AST1")
4       1     version          = 1
5       1     type             (see below)
6       2     flags            (bit 0 = more scan results follow)
8       4     length           = number of bytes in `body`
12      4     crc32            = crc32(body)  (IEEE 802.3, reflected 0xEDB88320)
16      N     body             (`length` bytes)
```

- `max_frame_size` is a per-node configuration limit (default 1 MiB). A frame
  whose `length` exceeds it is rejected with `ErrorResp` and the connection is
  closed (documented backpressure).
- The `crc32` covers the body only.
- Unknown `type` values are answered with `ErrorResp(unsupported)`.
- A truncated or corrupt frame (bad magic, bad version, bad crc, length past
  end) causes the connection to be closed.

## 2. Message types

| type | name          | direction        | body |
|------|---------------|------------------|------|
| 0x01 | Ping          | client -> node   | (empty) |
| 0x02 | Pong          | node -> client   | (empty) |
| 0x03 | StatusReq     | client -> node   | (empty) |
| 0x04 | StatusResp    | node -> client   | UTF-8 JSON string |
| 0x10 | AppendReq     | client -> node   | record body (event) |
| 0x11 | AppendResp    | node -> client   | code:u8, sequence:u64 |
| 0x12 | GetReq        | client -> node   | tenant:u64, key:string |
| 0x13 | GetResp       | node -> client   | code:u8, [record body] |
| 0x14 | ScanReq       | client -> node   | tenant:u64, from_ts:u64, to_ts:u64, max_results:u32 |
| 0x15 | ScanResp      | node -> client   | record body (one event) |
| 0x16 | ScanEnd       | node -> client   | reason:u8 |
| 0x17 | DeleteReq     | client -> node   | record body (deleted=true) |
| 0x20 | ShutdownReq   | client -> node   | (empty) |
| 0x21 | ShutdownResp  | node -> client   | UTF-8 string |
| 0x30 | ReplicateReq  | leader -> follower | partition:u32, record body |
| 0x31 | ReplicateAck  | follower -> leader | partition:u32, ok:u8 |
| 0x32 | SyncReq       | follower -> leader | partition:u32 |
| 0x33 | SyncResp      | leader -> follower | record body (one event) |
| 0x34 | SyncEnd       | leader -> follower | count:u32 |
| 0x7F | ErrorResp     | node -> client   | code:u8, message:string |

### 2.1 Record body

A record body encodes one event (shared by AppendReq, GetResp, ScanResp,
ReplicateReq, SyncResp):

```
kind:u8          = 1 (event)
tenant_id:u64
key_len:u32, key bytes
timestamp:u64    (microseconds since Unix epoch, UTC)
sequence:u64     (per-key ordering number)
event_id:u8[16]  (idempotency key)
deleted:u8       (0/1; 1 => tombstone, payload empty)
payload_len:u32, payload bytes
```

### 2.2 Consistency levels

Writes use the node's configured default consistency level (`one`, `quorum`,
`all`). `AppendResp.code` is:

- `0` ok
- `1` duplicate (idempotent retry; the event already exists)
- otherwise an error code (see `Err` in `src/common/result.hpp`). Notably
  `not_leader` and `quorum_unavailable` are reported honestly.

## 3. Scan behavior

A scan is streamed as zero or more `ScanResp` frames followed by exactly one
`ScanEnd`. `ScanEnd.reason` is `0` complete, `1` limit reached (more results
may exist; re-issue with a higher `from_ts`), or `2` cancelled. Results are
bounded by `max_results`. If the client disconnects mid-scan, the server stops
emitting (cancellation). Because data is append-only, a concurrent write during
a scan may or may not appear in that scan (no snapshot isolation; see the
architecture document).

## 4. Replication messages

- `ReplicateReq` carries the leader-assigned event (with its final `sequence`
  and `timestamp`) plus the partition number. A follower applies it
  idempotently and replies `ReplicateAck(ok=0)`.
- `SyncReq(partition)` asks a leader for a full catch-up of that partition.
  The leader streams all of that partition's events as `SyncResp` frames and
  terminates with `SyncEnd(count)`.
- All replication bodies share the same size limits and checksums as client
  messages.
