#define _GNU_SOURCE
#include "ckvstore.h"

#include "ckv_internal.h"
#include "ckv_index.h"
#include "ckv_io.h"
#include "ckv_log.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

struct ckv_store {
    int fd;
    char *path;
    struct ckv_config cfg;
    struct ckv_codec_limits lim;
    struct ckv_index ix;
    uint32_t since_fsync;
};

static enum ckv_err map_errno_io(void) {
    return CKV_EIO;
}

const char *ckv_strerror(enum ckv_err e) {
    switch (e) {
    case CKV_OK: return "OK";
    case CKV_EINVAL: return "invalid argument";
    case CKV_ENOMEM: return "out of memory";
    case CKV_EIO: return "I/O error";
    case CKV_ENOT_FOUND: return "not found";
    case CKV_ECORRUPT: return "corrupt data";
    case CKV_EFULL: return "limit exceeded";
    case CKV_EBUF_TOO_SMALL: return "buffer too small";
    default: return "unknown error";
    }
}

static enum ckv_err maybe_fsync(struct ckv_store *st) {
    if (st->cfg.fsync_policy == CKV_FSYNC_OFF) return CKV_OK;
    if (st->cfg.fsync_policy == CKV_FSYNC_ON_COMMIT) {
        if (ckv_fsync_file(st->fd) != 0) return map_errno_io();
        return CKV_OK;
    }
    /* periodic */
    st->since_fsync++;
    uint32_t period = st->cfg.fsync_period_records ? st->cfg.fsync_period_records : 1024u;
    if (st->since_fsync >= period) {
        if (ckv_fsync_file(st->fd) != 0) return map_errno_io();
        st->since_fsync = 0;
    }
    return CKV_OK;
}

static enum ckv_err validate_cfg(const struct ckv_config *cfg) {
    if (!cfg || !cfg->path) return CKV_EINVAL;
    if (cfg->max_key_bytes == 0 || cfg->max_value_bytes == 0) return CKV_EINVAL;
    if (cfg->max_index_entries == 0) return CKV_EINVAL;
    if (cfg->initial_index_capacity == 0) return CKV_EINVAL;
    if (!(cfg->fsync_policy == CKV_FSYNC_OFF ||
          cfg->fsync_policy == CKV_FSYNC_ON_COMMIT ||
          cfg->fsync_policy == CKV_FSYNC_PERIODIC)) return CKV_EINVAL;
    return CKV_OK;
}

static enum ckv_err rebuild_index(struct ckv_store *st) {
    off_t off = 0;
    uint8_t type = 0;
    uint32_t klen = 0, vlen = 0;

    unsigned char *kbuf = (unsigned char *)malloc(st->cfg.max_key_bytes ? st->cfg.max_key_bytes : 1);
    unsigned char *vbuf = (unsigned char *)malloc(st->cfg.max_value_bytes ? st->cfg.max_value_bytes : 1);
    if (!kbuf || !vbuf) {
        free(kbuf); free(vbuf);
        return CKV_ENOMEM;
    }

    while (1) {
        off_t rec_off = off;
        int rc = ckv_log_read_next(st->fd, &st->lim, &off, &type, &klen, &vlen, kbuf, vbuf);
        if (rc == 1) break;       /* EOF */
        if (rc < 0) break;        /* corrupt/incomplete tail: tolerate */
        uint64_t h = ckv_hash64(kbuf, klen);
        if (type == CKV_REC_PUT) {
            int new_key = 0;
            if (st->ix.size >= st->cfg.max_index_entries) {
                free(kbuf); free(vbuf);
                return CKV_EFULL;
            }
            if (!ckv_index_put(&st->ix, kbuf, klen, h, vlen, rec_off, &new_key)) {
                free(kbuf); free(vbuf);
                return CKV_ENOMEM;
            }
        } else if (type == CKV_REC_DEL) {
            (void)ckv_index_del(&st->ix, kbuf, klen, h);
        }
    }

    free(kbuf);
    free(vbuf);
    return CKV_OK;
}

enum ckv_err ckv_open(const struct ckv_config *cfg, struct ckv_store **out) {
    if (!out) return CKV_EINVAL;
    *out = NULL;

    enum ckv_err ve = validate_cfg(cfg);
    if (ve != CKV_OK) return ve;

    struct ckv_store *st = (struct ckv_store *)calloc(1, sizeof(*st));
    if (!st) return CKV_ENOMEM;

    st->cfg = *cfg;
    st->lim.max_key = cfg->max_key_bytes;
    st->lim.max_val = cfg->max_value_bytes;
    st->since_fsync = 0;

    st->path = strdup(cfg->path);
    if (!st->path) { free(st); return CKV_ENOMEM; }

    int fd = open(cfg->path, O_RDWR | O_CREAT | O_CLOEXEC, 0644);
    if (fd < 0) { free(st->path); free(st); return map_errno_io(); }
    st->fd = fd;

    if (ckv_index_init(&st->ix, cfg->initial_index_capacity) != 0) {
        close(st->fd); free(st->path); free(st); return CKV_ENOMEM;
    }

    enum ckv_err re = rebuild_index(st);
    if (re != CKV_OK) {
        ckv_close(st);
        return re;
    }

    *out = st;
    return CKV_OK;
}

void ckv_close(struct ckv_store *st) {
    if (!st) return;
    if (st->fd >= 0) (void)close(st->fd);
    ckv_index_free(&st->ix);
    free(st->path);
    free(st);
}

static enum ckv_err do_put_or_del(struct ckv_store *st,
                                 int is_put,
                                 const void *key, size_t klen,
                                 const void *val, size_t vlen) {
    if (!st || !key) return CKV_EINVAL;
    if (klen == 0 || klen > st->cfg.max_key_bytes) return CKV_EINVAL;
    if (is_put) {
        if (!val && vlen != 0) return CKV_EINVAL;
        if (vlen > st->cfg.max_value_bytes) return CKV_EINVAL;
    }

    uint64_t h = ckv_hash64(key, klen);
    off_t rec_off = 0;
    int rc;
    if (is_put) {
        rc = ckv_log_append_put(st->fd, &st->lim, key, (uint32_t)klen, val, (uint32_t)vlen, &rec_off);
    } else {
        rc = ckv_log_append_del(st->fd, &st->lim, key, (uint32_t)klen, &rec_off);
    }
    if (rc != 0) return map_errno_io();

    enum ckv_err fe = maybe_fsync(st);
    if (fe != CKV_OK) return fe;

    if (is_put) {
        int new_key = 0;
        if (!ckv_index_find(&st->ix, key, klen, h) && st->ix.size >= st->cfg.max_index_entries) {
            /* index limit exceeded: caller can compact/reopen with larger max */
            return CKV_EFULL;
        }
        if (!ckv_index_put(&st->ix, key, klen, h, (uint32_t)vlen, rec_off, &new_key)) return CKV_ENOMEM;
        return CKV_OK;
    } else {
        (void)ckv_index_del(&st->ix, key, klen, h);
        return CKV_OK;
    }
}

enum ckv_err ckv_put(struct ckv_store *st,
                     const void *key, size_t klen,
                     const void *val, size_t vlen) {
    return do_put_or_del(st, 1, key, klen, val, vlen);
}

enum ckv_err ckv_del(struct ckv_store *st, const void *key, size_t klen) {
    return do_put_or_del(st, 0, key, klen, NULL, 0);
}

enum ckv_err ckv_get(struct ckv_store *st,
                     const void *key, size_t klen,
                     void *out_buf, size_t out_cap,
                     size_t *out_len) {
    if (!st || !key || !out_len) return CKV_EINVAL;
    if (klen == 0 || klen > st->cfg.max_key_bytes) return CKV_EINVAL;

    uint64_t h = ckv_hash64(key, klen);
    struct ckv_index_entry *e = ckv_index_find(&st->ix, key, klen, h);
    if (!e) return CKV_ENOT_FOUND;

    /* Read record header at e->rec_off, validate, then read value directly. */
    uint8_t hdrb[18];
    if (ckv_pread_full(st->fd, hdrb, sizeof(hdrb), e->rec_off) != 0) return CKV_EIO;

    struct ckv_rec_hdr hrd;
    ckv_rec_hdr_decode(&hrd, hdrb);
    if (hrd.magic != CKV_MAGIC || hrd.version != CKV_VERSION || hrd.type != CKV_REC_PUT) return CKV_ECORRUPT;
    if (hrd.key_len != e->klen || hrd.val_len != e->vlen) return CKV_ECORRUPT;

    off_t k_off = e->rec_off + (off_t)sizeof(hdrb);
    /* Verify key bytes in file to avoid silent index corruption. */
    if (e->klen) {
        unsigned char *kbuf = (unsigned char *)malloc(e->klen);
        if (!kbuf) return CKV_ENOMEM;
        if (ckv_pread_full(st->fd, kbuf, e->klen, k_off) != 0) { free(kbuf); return CKV_EIO; }
        int ok = memcmp(kbuf, key, klen) == 0;
        free(kbuf);
        if (!ok) return CKV_ECORRUPT;
    }
    off_t v_off = k_off + (off_t)e->klen;

    *out_len = (size_t)e->vlen;
    if (!out_buf) return CKV_OK;
    if (out_cap < (size_t)e->vlen) return CKV_EBUF_TOO_SMALL;
    if (e->vlen && ckv_pread_full(st->fd, out_buf, e->vlen, v_off) != 0) return CKV_EIO;

    return CKV_OK;
}

enum ckv_err ckv_compact(struct ckv_store *st) {
    if (!st) return CKV_EINVAL;

    /* Create temp file in same directory. */
    char tmp[4096];
    int n = snprintf(tmp, sizeof(tmp), "%s.tmp.%ld", st->path, (long)getpid());
    if (n < 0 || (size_t)n >= sizeof(tmp)) return CKV_EIO;

    int tfd = open(tmp, O_RDWR | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (tfd < 0) return CKV_EIO;

    /* Rewrite live keys. Update offsets in-place. */
    off_t off = 0;
    for (size_t i = 0; i < st->ix.cap; i++) {
        struct ckv_index_entry *e = &st->ix.tab[i];
        if (e->in_use != 1) continue;

        /* Fetch value from old log at e->rec_off. */
        uint8_t hdrb[18];
        if (ckv_pread_full(st->fd, hdrb, sizeof(hdrb), e->rec_off) != 0) { close(tfd); (void)unlink(tmp); return CKV_EIO; }
        struct ckv_rec_hdr hrd;
        ckv_rec_hdr_decode(&hrd, hdrb);
        if (hrd.magic != CKV_MAGIC || hrd.version != CKV_VERSION || hrd.type != CKV_REC_PUT) { close(tfd); (void)unlink(tmp); return CKV_ECORRUPT; }
        if (hrd.key_len != e->klen || hrd.val_len != e->vlen) { close(tfd); (void)unlink(tmp); return CKV_ECORRUPT; }

        off_t v_off = e->rec_off + (off_t)sizeof(hdrb) + (off_t)e->klen;
        unsigned char *vbuf = (unsigned char *)malloc(e->vlen ? e->vlen : 1);
        if (!vbuf) { close(tfd); (void)unlink(tmp); return CKV_ENOMEM; }
        if (e->vlen && ckv_pread_full(st->fd, vbuf, e->vlen, v_off) != 0) { free(vbuf); close(tfd); (void)unlink(tmp); return CKV_EIO; }

        off_t new_off = 0;
        if (ckv_log_append_put(tfd, &st->lim, e->key, e->klen, vbuf, e->vlen, &new_off) != 0) {
            free(vbuf); close(tfd); (void)unlink(tmp); return CKV_EIO;
        }
        free(vbuf);
        e->rec_off = new_off; /* new offset in compacted file */
        off = new_off;
        (void)off;
    }

    /* Ensure new file is durable. */
    if (ckv_fsync_file(tfd) != 0) { close(tfd); (void)unlink(tmp); return CKV_EIO; }
    if (close(tfd) != 0) { (void)unlink(tmp); return CKV_EIO; }

    /* Atomic replace old log. */
    if (rename(tmp, st->path) != 0) { (void)unlink(tmp); return CKV_EIO; }
    (void)ckv_fsync_dir_of_path(st->path);

    /* Reopen store fd to refer to new file content. */
    int nfd = open(st->path, O_RDWR | O_CLOEXEC, 0644);
    if (nfd < 0) return CKV_EIO;
    (void)close(st->fd);
    st->fd = nfd;

    /* Reset fsync counter. */
    st->since_fsync = 0;

    return CKV_OK;
}

enum ckv_err ckv_iter_begin(struct ckv_store *st, struct ckv_iter *it) {
    if (!st || !it) return CKV_EINVAL;
    it->_pos = 0;
    return CKV_OK;
}

enum ckv_err ckv_iter_next(struct ckv_store *st, struct ckv_iter *it,
                           void *key_buf, size_t key_cap, size_t *key_len) {
    if (!st || !it || !key_len) return CKV_EINVAL;
    while (it->_pos < st->ix.cap) {
        struct ckv_index_entry *e = &st->ix.tab[it->_pos++];
        if (e->in_use != 1) continue;
        *key_len = (size_t)e->klen;
        if (!key_buf) return CKV_OK;
        if (key_cap < (size_t)e->klen) return CKV_EBUF_TOO_SMALL;
        if (e->klen) memcpy(key_buf, e->key, e->klen);
        return CKV_OK;
    }
    return CKV_ENOT_FOUND;
}

void ckv_iter_end(struct ckv_store *st, struct ckv_iter *it) {
    (void)st;
    if (!it) return;
    it->_pos = 0;
}
