#define _GNU_SOURCE
#include "ckv_log.h"
#include "ckv_crc32.h"
#include "ckv_io.h"

#include <errno.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

size_t ckv_rec_encoded_header_bytes(void) { return 18u; }

void ckv_rec_hdr_encode(uint8_t out[18], const struct ckv_rec_hdr *h) {
    ckv_le32enc(&out[0], h->magic);
    out[4] = h->version;
    out[5] = h->type;
    ckv_le32enc(&out[6], h->key_len);
    ckv_le32enc(&out[10], h->val_len);
    ckv_le32enc(&out[14], h->crc32);
}

void ckv_rec_hdr_decode(struct ckv_rec_hdr *h, const uint8_t in[18]) {
    h->magic = ckv_le32dec(&in[0]);
    h->version = in[4];
    h->type = in[5];
    h->key_len = ckv_le32dec(&in[6]);
    h->val_len = ckv_le32dec(&in[10]);
    h->crc32 = ckv_le32dec(&in[14]);
}

uint32_t ckv_rec_crc_v1(uint8_t type, uint32_t klen, uint32_t vlen,
                        const void *key, const void *val) {
    uint32_t crc = ckv_crc32_init();
    crc = ckv_crc32_update(crc, &type, 1);
    uint8_t le[4];
    ckv_le32enc(le, klen);
    crc = ckv_crc32_update(crc, le, 4);
    ckv_le32enc(le, vlen);
    crc = ckv_crc32_update(crc, le, 4);
    if (klen && key) crc = ckv_crc32_update(crc, key, klen);
    if (vlen && val) crc = ckv_crc32_update(crc, val, vlen);
    return ckv_crc32_finish(crc);
}

static int get_eof_offset(int fd, off_t *out) {
    struct stat st;
    if (fstat(fd, &st) != 0) return -1;
    *out = st.st_size;
    return 0;
}

int ckv_log_append_put(int fd, const struct ckv_codec_limits *lim,
                       const void *key, uint32_t klen,
                       const void *val, uint32_t vlen,
                       off_t *out_off) {
    if (klen > lim->max_key || vlen > lim->max_val) return -1;
    off_t off0;
    if (get_eof_offset(fd, &off0) != 0) return -1;
    if (off0 < 0 || (uint64_t)off0 > INT64_MAX - 18u - (uint64_t)klen - vlen)
        return -1;

    struct ckv_rec_hdr h;
    h.magic = CKV_MAGIC;
    h.version = CKV_VERSION;
    h.type = CKV_REC_PUT;
    h.key_len = klen;
    h.val_len = vlen;
    h.crc32 = ckv_rec_crc_v1(h.type, klen, vlen, key, val);

    uint8_t hdr[18];
    ckv_rec_hdr_encode(hdr, &h);

    off_t off = off0;
    if (ckv_pwrite_full(fd, hdr, sizeof(hdr), off) != 0) return -1;
    off += (off_t)sizeof(hdr);
    if (klen && ckv_pwrite_full(fd, key, klen, off) != 0) return -1;
    off += (off_t)klen;
    if (vlen && ckv_pwrite_full(fd, val, vlen, off) != 0) return -1;

    if (out_off) *out_off = off0;
    return 0;
}

int ckv_log_append_del(int fd, const struct ckv_codec_limits *lim,
                       const void *key, uint32_t klen,
                       off_t *out_off) {
    if (klen > lim->max_key) return -1;
    off_t off0;
    if (get_eof_offset(fd, &off0) != 0) return -1;
    if (off0 < 0 || (uint64_t)off0 > INT64_MAX - 18u - (uint64_t)klen)
        return -1;

    struct ckv_rec_hdr h;
    h.magic = CKV_MAGIC;
    h.version = CKV_VERSION;
    h.type = CKV_REC_DEL;
    h.key_len = klen;
    h.val_len = 0;
    h.crc32 = ckv_rec_crc_v1(h.type, klen, 0, key, NULL);

    uint8_t hdr[18];
    ckv_rec_hdr_encode(hdr, &h);

    off_t off = off0;
    if (ckv_pwrite_full(fd, hdr, sizeof(hdr), off) != 0) return -1;
    off += (off_t)sizeof(hdr);
    if (klen && ckv_pwrite_full(fd, key, klen, off) != 0) return -1;

    if (out_off) *out_off = off0;
    return 0;
}

int ckv_log_read_next(int fd, const struct ckv_codec_limits *lim,
                      off_t *io_off,
                      uint8_t *out_type,
                      uint32_t *out_klen,
                      uint32_t *out_vlen,
                      void *key_buf,
                      void *val_buf) {
    uint8_t hdrb[18];
    off_t off = *io_off;

    /* Distinguish an incomplete tail from corruption and read errors. */
    off_t end;
    if (get_eof_offset(fd, &end) != 0) return -2;
    if (off < 0 || off > end) return -1;
    if (off == end) return 1;
    if (end - off < (off_t)sizeof(hdrb)) return 2;
    if (ckv_pread_full(fd, hdrb, sizeof(hdrb), off) != 0) return -2;

    struct ckv_rec_hdr h;
    ckv_rec_hdr_decode(&h, hdrb);
    if (h.magic != CKV_MAGIC || h.version != CKV_VERSION) return -1;
    if (!(h.type == CKV_REC_PUT || h.type == CKV_REC_DEL)) return -1;
    if (h.key_len == 0) return -1;
    if (h.key_len > lim->max_key) return -3;
    if (h.type == CKV_REC_DEL && h.val_len != 0) return -1;
    if (h.type == CKV_REC_PUT && h.val_len > lim->max_val) return -3;
    if ((uint64_t)h.key_len + h.val_len >
        (uint64_t)(end - off - (off_t)sizeof(hdrb))) return 2;

    off_t payload_off = off + (off_t)sizeof(hdrb);
    uint32_t klen = h.key_len;
    uint32_t vlen = h.val_len;

    /* Read payload (or skip). */
    if (klen) {
        if (key_buf) {
            if (ckv_pread_full(fd, key_buf, klen, payload_off) != 0) return -2;
        } else {
            uint8_t tmp[256];
            uint32_t left = klen;
            off_t o = payload_off;
            while (left) {
                size_t chunk = left > sizeof(tmp) ? sizeof(tmp) : (size_t)left;
                if (ckv_pread_full(fd, tmp, chunk, o) != 0) return -2;
                o += (off_t)chunk;
                left -= (uint32_t)chunk;
            }
        }
        payload_off += (off_t)klen;
    }
    if (vlen) {
        if (val_buf) {
            if (ckv_pread_full(fd, val_buf, vlen, payload_off) != 0) return -2;
        } else {
            uint8_t tmp[256];
            uint32_t left = vlen;
            off_t o = payload_off;
            while (left) {
                size_t chunk = left > sizeof(tmp) ? sizeof(tmp) : (size_t)left;
                if (ckv_pread_full(fd, tmp, chunk, o) != 0) return -2;
                o += (off_t)chunk;
                left -= (uint32_t)chunk;
            }
        }
        payload_off += (off_t)vlen;
    }

    /* Verify CRC. If buffers weren't provided, recompute by reading again in chunks. */
    uint32_t crc = ckv_crc32_init();
    crc = ckv_crc32_update(crc, &h.type, 1);
    uint8_t le[4];
    ckv_le32enc(le, klen);
    crc = ckv_crc32_update(crc, le, 4);
    ckv_le32enc(le, vlen);
    crc = ckv_crc32_update(crc, le, 4);

    off_t data_off = off + (off_t)sizeof(hdrb);
    if (klen) {
        if (key_buf) {
            crc = ckv_crc32_update(crc, key_buf, klen);
        } else {
            uint8_t tmp[256];
            uint32_t left = klen;
            off_t o = data_off;
            while (left) {
                size_t chunk = left > sizeof(tmp) ? sizeof(tmp) : (size_t)left;
                if (ckv_pread_full(fd, tmp, chunk, o) != 0) return -2;
                crc = ckv_crc32_update(crc, tmp, chunk);
                o += (off_t)chunk;
                left -= (uint32_t)chunk;
            }
        }
        data_off += (off_t)klen;
    }
    if (vlen) {
        if (val_buf) {
            crc = ckv_crc32_update(crc, val_buf, vlen);
        } else {
            uint8_t tmp[256];
            uint32_t left = vlen;
            off_t o = data_off;
            while (left) {
                size_t chunk = left > sizeof(tmp) ? sizeof(tmp) : (size_t)left;
                if (ckv_pread_full(fd, tmp, chunk, o) != 0) return -2;
                crc = ckv_crc32_update(crc, tmp, chunk);
                o += (off_t)chunk;
                left -= (uint32_t)chunk;
            }
        }
    }
    crc = ckv_crc32_finish(crc);
    if (crc != h.crc32) return -1;

    *io_off = off + (off_t)sizeof(hdrb) + (off_t)klen + (off_t)vlen;
    if (out_type) *out_type = h.type;
    if (out_klen) *out_klen = klen;
    if (out_vlen) *out_vlen = vlen;
    return 0;
}
