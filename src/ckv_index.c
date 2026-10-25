#include "ckv_index.h"

#include <stdlib.h>
#include <string.h>

static size_t round_up_pow2(size_t x) {
    size_t p = 1;
    while (p < x) {
        if (p > SIZE_MAX / 2) return 0;
        p <<= 1;
    }
    return p;
}

/* FNV-1a 64-bit */
uint64_t ckv_hash64(const void *data, size_t len) {
    const unsigned char *p = (const unsigned char *)data;
    uint64_t h = 1469598103934665603ull;
    for (size_t i = 0; i < len; i++) {
        h ^= (uint64_t)p[i];
        h *= 1099511628211ull;
    }
    return h;
}

int ckv_index_init(struct ckv_index *ix, size_t cap) {
    cap = round_up_pow2(cap < 16 ? 16 : cap);
    if (cap == 0 || cap > SIZE_MAX / sizeof(*ix->tab)) return -1;
    ix->tab = (struct ckv_index_entry *)calloc(cap, sizeof(*ix->tab));
    if (!ix->tab) return -1;
    ix->cap = cap;
    ix->size = 0;
    for (size_t i = 0; i < cap; i++) ix->tab[i].in_use = 0;
    return 0;
}

void ckv_index_free(struct ckv_index *ix) {
    if (!ix || !ix->tab) return;
    for (size_t i = 0; i < ix->cap; i++) {
        if (ix->tab[i].in_use == 1) free(ix->tab[i].key);
    }
    free(ix->tab);
    ix->tab = NULL;
    ix->cap = 0;
    ix->size = 0;
}

static int keyeq(const struct ckv_index_entry *e, const void *key, size_t klen) {
    return e->klen == (uint32_t)klen && memcmp(e->key, key, klen) == 0;
}

struct ckv_index_entry *ckv_index_find(struct ckv_index *ix,
                                       const void *key, size_t klen,
                                       uint64_t hash) {
    if (!ix->tab) return NULL;
    size_t mask = ix->cap - 1;
    size_t i = (size_t)hash & mask;
    for (size_t step = 0; step < ix->cap; step++) {
        struct ckv_index_entry *e = &ix->tab[i];
        if (e->in_use == 0) return NULL;
        if (e->in_use == 1 && e->hash == hash && keyeq(e, key, klen)) return e;
        i = (i + 1) & mask;
    }
    return NULL;
}

static struct ckv_index_entry *find_slot(struct ckv_index *ix,
                                        const void *key, size_t klen,
                                        uint64_t hash,
                                        int *out_found_existing) {
    size_t mask = ix->cap - 1;
    size_t i = (size_t)hash & mask;
    struct ckv_index_entry *first_tomb = NULL;
    for (size_t step = 0; step < ix->cap; step++) {
        struct ckv_index_entry *e = &ix->tab[i];
        if (e->in_use == 0) {
            if (out_found_existing) *out_found_existing = 0;
            return first_tomb ? first_tomb : e;
        }
        if (e->in_use == -1) {
            if (!first_tomb) first_tomb = e;
        } else if (e->hash == hash && keyeq(e, key, klen)) {
            if (out_found_existing) *out_found_existing = 1;
            return e;
        }
        i = (i + 1) & mask;
    }
    if (out_found_existing) *out_found_existing = 0;
    return first_tomb; /* table full; should not happen if resized properly */
}

static int should_grow(const struct ckv_index *ix) {
    /* Maintain load factor <= ~0.7 */
    return (ix->size + 1) * 10 >= ix->cap * 7;
}

int ckv_index_rehash(struct ckv_index *ix, size_t new_cap) {
    struct ckv_index newix;
    if (ckv_index_init(&newix, new_cap) != 0) return -1;
    for (size_t i = 0; i < ix->cap; i++) {
        struct ckv_index_entry *e = &ix->tab[i];
        if (e->in_use == 1) {
            int found = 0;
            struct ckv_index_entry *dst = find_slot(&newix, e->key, e->klen, e->hash, &found);
            (void)found;
            *dst = *e;
            dst->in_use = 1;
            newix.size++;
            /* prevent old free from freeing moved key */
            e->key = NULL;
            e->in_use = 0;
        }
    }
    ckv_index_free(ix);
    *ix = newix;
    return 0;
}

struct ckv_index_entry *ckv_index_put(struct ckv_index *ix,
                                      const void *key, size_t klen,
                                      uint64_t hash,
                                      uint32_t vlen, off_t rec_off,
                                      int *out_new_key) {
    struct ckv_index_entry *existing = ckv_index_find(ix, key, klen, hash);
    if (existing) {
        existing->vlen = vlen;
        existing->rec_off = rec_off;
        if (out_new_key) *out_new_key = 0;
        return existing;
    }
    if (should_grow(ix)) {
        if (ix->cap > SIZE_MAX / 2) return NULL;
        if (ckv_index_rehash(ix, ix->cap * 2) != 0) return NULL;
    }

    int found = 0;
    struct ckv_index_entry *e = find_slot(ix, key, klen, hash, &found);
    if (!e) return NULL;

    if (!found) {
        unsigned char *k = (unsigned char *)malloc(klen ? klen : 1);
        if (!k) return NULL;
        if (klen) memcpy(k, key, klen);
        e->key = k;
        e->klen = (uint32_t)klen;
        e->hash = hash;
        e->in_use = 1;
        ix->size++;
        if (out_new_key) *out_new_key = 1;
    } else {
        if (out_new_key) *out_new_key = 0;
    }

    e->vlen = vlen;
    e->rec_off = rec_off;
    return e;
}

int ckv_index_del(struct ckv_index *ix,
                  const void *key, size_t klen,
                  uint64_t hash) {
    struct ckv_index_entry *e = ckv_index_find(ix, key, klen, hash);
    if (!e) return 0;
    free(e->key);
    e->key = NULL;
    e->in_use = -1; /* tombstone */
    ix->size--;
    return 1;
}
