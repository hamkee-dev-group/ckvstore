# ckvstore (v1)

Small embeddable key/value store library in pure C (C11) for Linux.

## Build

```sh
make
make test
```

## CLI

```sh
./ckvcli /tmp/store.log put key value
./ckvcli /tmp/store.log get key
./ckvcli /tmp/store.log del key
./ckvcli /tmp/store.log compact
```

Keys/values via CLI are treated as UTF-8 strings; the library API treats them as binary blobs.

## Notes

- Single-file append-only log with PUT/DEL records
- In-memory hash index (open addressing)
- Crash recovery via log scan (trailing partial/corrupt record tolerated)
- Explicit compaction via `ckv_compact()`
