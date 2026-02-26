#include "ckv_crc32.h"

/* Standard CRC-32 (IEEE 802.3) polynomial 0xEDB88320 */
static uint32_t table[256];
static int table_inited = 0;

static void init_table(void) {
    for (uint32_t i = 0; i < 256u; i++) {
        uint32_t c = i;
        for (uint32_t k = 0; k < 8u; k++) {
            if (c & 1u) c = 0xEDB88320u ^ (c >> 1);
            else c = c >> 1;
        }
        table[i] = c;
    }
    table_inited = 1;
}

uint32_t ckv_crc32_init(void) {
    if (!table_inited) init_table();
    return 0xFFFFFFFFu;
}

uint32_t ckv_crc32_update(uint32_t crc, const void *data, size_t len) {
    const unsigned char *p = (const unsigned char *)data;
    for (size_t i = 0; i < len; i++) {
        crc = table[(crc ^ (uint32_t)p[i]) & 0xFFu] ^ (crc >> 8);
    }
    return crc;
}

uint32_t ckv_crc32_finish(uint32_t crc) {
    return crc ^ 0xFFFFFFFFu;
}
