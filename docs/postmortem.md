# Postmortem: Connection lifetime in the IOCP server

## The bug

`net_end_to_end` passed, but `net_oversized_frame_rejected` hung indefinitely
the first time it ran. The symptom was a process that never printed the test
result and had to be killed.

The root cause was in the IOCP server's connection lifetime. The `Connection`
object was managed by **two independent ownership mechanisms at once**:

1. a manual reference count (`Connection::refs`, an `std::atomic<int>` starting
   at 1), incremented per outstanding overlapped op and per worker job, and
2. `std::shared_ptr<Connection>`, held by the connection table and by worker
   jobs (obtained via `std::enable_shared_from_this`).

The manual counter's "1" was meant to represent the table's reference, but the
table actually held a `shared_ptr`. Both mechanisms called `delete this`/`delete
c` independently when they believed they were the last owner, and neither was
coordinated with the other. In the oversized-frame path (server detects an
oversized frame, sends an error, and closes the connection while a recv and a
send are still in flight), the two counters could both reach zero, leading to a
double-free / use-after-free. The result was memory corruption that surfaced as
a hang (and, under other runs, could have been a crash).

A secondary, related bug was exposed while investigating: the frame decoder
only checked `length > available` instead of `length + 4 > available`, so a
frame truncated by a few bytes was read past its declared length (and past the
validated bounds). This was caught by `record_truncation_detected` only after
the checksum happened to be recomputed over memory that was still there.

## How I fixed it

1. **One lifetime mechanism.** I removed `shared_ptr<Connection>` entirely for
   the connection object and kept only the manual refcount. The table holds one
   reference; each pending overlapped op (recv/send) holds one; each queued
   worker job holds one. `release()` deletes the object exactly once, when the
   count reaches zero. This also removed the need for `enable_shared_from_this`
   and the `std::bad_weak_ptr` hazard that came with it. The cancellation token
   (`std::shared_ptr<std::atomic<bool>>`) remains separate and independent of
   connection lifetime, so a handler that outlives a closed connection can still
   check cancellation safely.

2. **Correct frame bound check.** Changed the decoder to require
   `length + 4 <= available` (a frame is `length` field + crc + body = `length + 4`
   bytes), so truncated frames are rejected instead of being read past their
   bounds.

## Lessons

- When mixing raw, non-blocking Winsock I/O with a connection object whose
  `OVERLAPPED` structs are embedded inline, pick *one* lifetime scheme and make
  every "reference" (table, op, job) feed that single counter. Two mechanisms
  that both think they own the object is the classic double-free.
- `-W4` and deterministic tests are cheap insurance, but a hang in a fault test
  is best diagnosed by confirming whether the process is *stuck* or *crashed*
  (exit code vs. timeout) before reading the code.
- A bounds check that is off by a constant ("length" vs "length + 4") can pass
  its own unit tests if the test buffer still happens to contain the full data.
  The test must *actually* shorten the buffer, not just pass a smaller length
  while the backing vector still holds everything.
