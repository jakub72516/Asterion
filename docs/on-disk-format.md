# Asterion On-Disk Format Specification

Version 1. All integers are little-endian. Checksums are CRC-32 (IEEE 802.3,
reflected polynomial `0xEDB88320`).

## 1. Directory layout

A node's `data_dir` contains a set of segment files:

```
segment_<16-hex-id>.astseg
```

`<16-hex-id>` is the segment id, a monotonically increasing `uint64`. Segments
are appended in id order; the highest-id *non-sealed* segment is the active
segment. No other files are written by the storage engine.

## 2. Segment file

A segment is a versioned header followed by zero or more records.

### 2.1 Segment header (48 bytes)

```
offset  size  field
0       4     magic        = 0x41534547 ("ASEG")
4       4     version      = 1
8       8     segment_id
16      8     created_ts   (microseconds since Unix epoch)
24      8     first_seq    (reserved, 0)
32      8     record_count (advisory; recomputed on recovery)
40      4     flags        (bit 0 = sealed)
44      4     header_crc   (crc32 of bytes 0..43)
```

`header_crc` must match; otherwise the segment is rejected as corrupt.

### 2.2 Record frame

Each record is length-delimited and checksummed:

```
length  u32   = 4 + len(payload)         (bytes of crc + payload)
crc     u32   = crc32(payload)
payload bytes  = the encoded record (see below)
```

A record whose `length` is `0` is a clean end-of-file sentinel (not normally
written). A record whose `length` is inconsistent, whose crc is wrong, or that
extends past the end of the file is a **corrupt/incomplete trailing record**.

### 2.3 Record payload (kind 1 = event)

```
kind:u8          = 1
tenant_id:u64
key_len:u32, key bytes
timestamp:u64
sequence:u64
event_id:u8[16]
deleted:u8
payload_len:u32, payload bytes
```

`deleted=1` marks a tombstone (payload empty).

## 3. Durability semantics on Windows

Asterion uses buffered I/O (`WriteFile` into the OS file cache) with explicit
`FlushFileBuffers` for durability. "Durable" means: after `FlushFileBuffers`
returns success, the file's data and metadata have been handed to the storage
device by the OS. This survives:

- **Process termination** (always, once the OS cache has the data), and
- **Operating-system crash / power loss** (because `FlushFileBuffers` is the
  Windows equivalent of `fsync`).

Caveats that are NOT covered, and are therefore not claimed:

- A storage device that lies about write completion (write-back cache without
  power protection) can still lose data on power loss. This is a hardware
  property, not something software can guarantee.
- `FlushFileBuffers` does not force the *directory entry* of a freshly created
  file to disk; Asterion calls `FlushFileBuffers` on a directory handle after
  creating a new segment to make segment creation durable.
- The `none` flush mode makes **no** durability guarantee (acknowledgement
  survives only process termination, not OS crash or power loss). `batch` mode
  flushes asynchronously on an interval; an acknowledgement in `batch` mode is
  durable once the flush that covers it completes.

## 4. Recovery

On startup the log:

1. Lists `*.astseg` files and parses their ids.
2. Opens each, validates the header (magic, version, crc).
3. Scans records in order. On the first invalid record (bad length, bad crc,
   truncation), the segment is truncated at the last good offset, isolating the
   corrupt/incomplete tail. Records before that point are preserved.
4. Recomputes the record count (the header's `record_count` may be stale after
   a crash).
5. Chooses the highest-id non-sealed segment as active (or creates a new one).

Recovery preserves valid acknowledged records and drops incomplete/corrupted
trailing records. It reports what it dropped for observability.

## 5. Rotation and compaction

- **Rotation**: when the active segment reaches `segment_size_bytes`, it is
  sealed (flag + header rewrite + flush) and a new segment is created.
- **Compaction** merges the oldest sealed segments into one, keeping only the
  latest (highest-sequence) version of each key and dropping tombstones.
  Positions change, so the in-memory index is rebuilt afterward.

Crash-safety note: compaction is not transactionally atomic. A crash between
deleting source segments and creating the merged segment can lose the compacted
records (the source data is read fully into memory first). This is documented,
not hidden; see `docs/architecture.md`.
