# ckvstore (v1)

Small embeddable key/value store library in C11 using POSIX file APIs.
Single-file append-only PUT/DEL log, in-memory hash index, and explicit compaction.
The public API and v1 on-disk encoding are unchanged by the correctness fixes.
No external libraries are required. Native Windows support is not included.

## Build

```sh
make
make test
```

Use a C11 compiler and 64-bit file offsets (the Makefile enables these on Linux).
Tests include reopen/corruption/limit regressions, a deterministic reference-map
workload, and injected allocation, read, write, sync, truncate and rename errors.
The fault-test binary compiles private instrumented copies of the implementation;
no fault-injection code is included in the production library.

## CLI

```sh
./ckvcli /tmp/store.log put key value
./ckvcli /tmp/store.log get key
./ckvcli /tmp/store.log del key
./ckvcli /tmp/store.log compact
```

CLI arguments are strings; the library API accepts binary keys and values,
including empty values. The CLI now uses `CKV_FSYNC_ON_COMMIT`.

## Operating contract

- Use one process and one open store handle per database file. Serialize calls
  on that handle. There is no file locking or internal thread synchronization.
  Independent files may be used from independent threads.
- Use a regular file at a stable path on a local filesystem. Do not use symlink
  aliases, rename the containing directory, change the working directory while
  a relative path is open, or modify the file externally while it is open.
- All live keys are retained in RAM. Opening scans the complete log. Compaction
  needs space for a second file and a temporary offset array proportional to
  index capacity. Keep configured limits large enough for the records in the log,
  including historical values and the peak live-key count during replay.
- The parent directory must already exist. Compaction retains ordinary permission
  bits, but does not preserve ownership, ACLs, extended attributes or hard links.
- Do not mutate the store while iterating. Size queries and short iterator
  buffers no longer consume a key; retry with a sufficiently large buffer.

## Recovery and errors

Opening truncates and syncs an incomplete final record before accepting writes.
A malformed complete record or CRC mismatch returns `CKV_ECORRUPT`; a read error
returns `CKV_EIO`. Records exceeding configured limits return `CKV_EFULL` without
truncation. The format has no separate header checksum: truncation is inferred
from the available bytes and the declared record length. This is recovery for
interrupted appends, not a repair tool for arbitrary corruption.

Writes rejected with `CKV_EINVAL`, `CKV_EFULL` or `CKV_ENOMEM` do not append records.
After a write I/O/sync error, the handle refuses further operations: close and
reopen it. The failed write has an uncertain outcome and may appear after recovery.
A complete value read verifies its CRC; a NULL-buffer size query does not read or
validate the value. Ignore output bytes when a read returns an error.

Compaction validates values and writes a uniquely created temporary file without
changing the live index. After syncing the temporary file, it renames it over the
log, switches to the already-open descriptor, and syncs the containing directory.
Before replacement, failures leave the original log/index intact. After replacement,
a sync/close failure disables the handle. Close/reopen after a compaction I/O error.
A process crash can leave an unreferenced `store.log.tmp.*` file; recovery ignores it.

## Durability

- `CKV_FSYNC_ON_COMMIT`: each successful put/delete has completed `fsync`.
- `CKV_FSYNC_PERIODIC`: sync every configured number of records (zero means 1024).
- `CKV_FSYNC_OFF`: no write durability barrier.

Close does not flush pending writes. Select ON_COMMIT when acknowledged writes
must be synced. Synchronous modes sync the parent directory during open so a
new file's directory entry is covered. Recovery repairs and compaction sync even
when normal writes use OFF. Filesystem sync errors are reported, not ignored.

These guarantees depend on filesystem and device sync behavior. This patch uses
POSIX `fsync`, not macOS-specific `F_FULLFSYNC`, and does not claim physical
power-loss certification. The failure tests exercise software error handling;
they do not simulate every storage device or filesystem failure mode.
