#ifndef CKV_CRC32_H
#define CKV_CRC32_H

#include <stddef.h>
#include <stdint.h>

uint32_t ckv_crc32_init(void);
uint32_t ckv_crc32_update(uint32_t crc, const void *data, size_t len);
uint32_t ckv_crc32_finish(uint32_t crc);

#endif
