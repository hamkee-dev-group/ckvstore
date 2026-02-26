#ifndef CKV_LOG_H
#define CKV_LOG_H

#include "ckv_internal.h"
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

struct ckv_codec_limits {
    uint32_t max_key;
    uint32_t max_val;
};

size_t ckv_rec_encoded_header_bytes(void);

/* Encode header to wire bytes. */
void ckv_rec_hdr_encode(uint8_t out[18], const struct ckv_rec_hdr *h);

/* Decode header from wire bytes. */
void ckv_rec_hdr_decode(struct ckv_rec_hdr *h, const uint8_t in[18]);

/* Compute v1 crc32 of (type|key_len|val_len|key|val). */
uint32_t ckv_rec_crc_v1(uint8_t type, uint32_t klen, uint32_t vlen,
                        const void *key, const void *val);

/* Append PUT/DEL record; returns offset where record starts. */
int ckv_log_append_put(int fd, const struct ckv_codec_limits *lim,
                       const void *key, uint32_t klen,
                       const void *val, uint32_t vlen,
                       off_t *out_off);

int ckv_log_append_del(int fd, const struct ckv_codec_limits *lim,
                       const void *key, uint32_t klen,
                       off_t *out_off);

/*
 * Read next record from file at offset *io_off. On success, fills type/klen/vlen
 * and optionally reads key/value into provided buffers (if non-NULL).
 * If key_buf is NULL, key bytes are skipped; same for val_buf.
 *
 * Returns:
 *  0 success
 *  1 EOF (no more complete records)
 * -1 corrupt/incomplete tail (stop scanning)
 */
int ckv_log_read_next(int fd, const struct ckv_codec_limits *lim,
                      off_t *io_off,
                      uint8_t *out_type,
                      uint32_t *out_klen,
                      uint32_t *out_vlen,
                      void *key_buf,
                      void *val_buf);

#endif
