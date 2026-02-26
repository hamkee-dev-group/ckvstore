#ifndef CKV_INDEX_H
#define CKV_INDEX_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

struct ckv_index_entry {
    uint64_t hash;
    uint32_t klen;
    uint32_t vlen;
    off_t rec_off;        /* offset of record start (PUT) */
    unsigned char *key;   /* owned key bytes; NULL means empty or tombstone */
    int in_use;           /* 1=live, 0=empty, -1=tombstone */
};

struct ckv_index {
    struct ckv_index_entry *tab;
    size_t cap;
    size_t size;
};

int ckv_index_init(struct ckv_index *ix, size_t cap);
void ckv_index_free(struct ckv_index *ix);

uint64_t ckv_hash64(const void *data, size_t len);

struct ckv_index_entry *ckv_index_find(struct ckv_index *ix,
                                       const void *key, size_t klen,
                                       uint64_t hash);

struct ckv_index_entry *ckv_index_put(struct ckv_index *ix,
                                      const void *key, size_t klen,
                                      uint64_t hash,
                                      uint32_t vlen, off_t rec_off,
                                      int *out_new_key);

int ckv_index_del(struct ckv_index *ix,
                  const void *key, size_t klen,
                  uint64_t hash);

int ckv_index_rehash(struct ckv_index *ix, size_t new_cap);

#endif
