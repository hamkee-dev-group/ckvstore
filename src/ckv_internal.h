#ifndef CKV_INTERNAL_H
#define CKV_INTERNAL_H

#include <stdint.h>
#include <stddef.h>
#include <sys/types.h>

_Static_assert(sizeof(off_t) >= 8, "ckvstore requires 64-bit file offsets");

#define CKV_MAGIC 0x434B5631u /* 'C''K''V''1' */

#define CKV_VERSION 1u

enum ckv_rec_type {
    CKV_REC_PUT = 1,
    CKV_REC_DEL = 2,
};

struct ckv_rec_hdr {
    uint32_t magic;
    uint8_t version;
    uint8_t type;
    uint32_t key_len;
    uint32_t val_len;
    uint32_t crc32;
};

static inline uint32_t ckv_le32dec(const uint8_t b[4]) {
    return (uint32_t)b[0]
        | ((uint32_t)b[1] << 8)
        | ((uint32_t)b[2] << 16)
        | ((uint32_t)b[3] << 24);
}

static inline void ckv_le32enc(uint8_t b[4], uint32_t v) {
    b[0] = (uint8_t)(v & 0xffu);
    b[1] = (uint8_t)((v >> 8) & 0xffu);
    b[2] = (uint8_t)((v >> 16) & 0xffu);
    b[3] = (uint8_t)((v >> 24) & 0xffu);
}

#endif
