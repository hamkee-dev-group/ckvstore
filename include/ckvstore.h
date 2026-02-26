#ifndef CKVSTORE_H
#define CKVSTORE_H

/*
 * ckvstore - small embeddable key/value store (append-only log + in-memory index)
 * Public API (v1)
 *
 * Keys and values are treated as binary blobs.
 * Single-writer, single-process semantics.
 */

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct ckv_store;

enum ckv_fsync_policy {
    CKV_FSYNC_OFF = 0,
    CKV_FSYNC_ON_COMMIT = 1,
    CKV_FSYNC_PERIODIC = 2,
};

enum ckv_err {
    CKV_OK = 0,
    CKV_EINVAL = 1,
    CKV_ENOMEM = 2,
    CKV_EIO = 3,
    CKV_ENOT_FOUND = 4,
    CKV_ECORRUPT = 5,
    CKV_EFULL = 6,
    CKV_EBUF_TOO_SMALL = 7,
};

const char *ckv_strerror(enum ckv_err e);

struct ckv_config {
    const char *path;                  /* path to the data file */
    uint32_t max_key_bytes;            /* hard limit, inclusive */
    uint32_t max_value_bytes;          /* hard limit, inclusive */
    uint32_t max_index_entries;        /* max in-memory index entries */
    uint32_t initial_index_capacity;   /* hash table capacity (will be rounded) */
    enum ckv_fsync_policy fsync_policy;
    uint32_t fsync_period_records;     /* used when fsync_policy == CKV_FSYNC_PERIODIC */
};

enum ckv_err ckv_open(const struct ckv_config *cfg, struct ckv_store **out);
void ckv_close(struct ckv_store *st);

enum ckv_err ckv_put(struct ckv_store *st,
                     const void *key, size_t klen,
                     const void *val, size_t vlen);

enum ckv_err ckv_get(struct ckv_store *st,
                     const void *key, size_t klen,
                     void *out_buf, size_t out_cap,
                     size_t *out_len);

enum ckv_err ckv_del(struct ckv_store *st, const void *key, size_t klen);

enum ckv_err ckv_compact(struct ckv_store *st);

/* Iterator over live keys (order unspecified). */
struct ckv_iter {
    size_t _pos;
};

enum ckv_err ckv_iter_begin(struct ckv_store *st, struct ckv_iter *it);
enum ckv_err ckv_iter_next(struct ckv_store *st, struct ckv_iter *it,
                           void *key_buf, size_t key_cap, size_t *key_len);
void ckv_iter_end(struct ckv_store *st, struct ckv_iter *it);

#ifdef __cplusplus
}
#endif

#endif /* CKVSTORE_H */
