/* crc32.c — reflected CRC-32 (IEEE) used by progress, bookmarks and upload.
 * Skeleton owner. No I/O, no state; host tests link this file directly.
 */
#include "rc_common.h"

uint32_t rc_crc32_update(uint32_t crc, const void *buf, size_t len)
{
    const uint8_t *p = (const uint8_t *)buf;
    crc = ~crc;
    while (len--) {
        crc ^= *p++;
        for (int i = 0; i < 8; i++) {
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
        }
    }
    return ~crc;
}

uint32_t rc_crc32_ieee(const void *buf, size_t len)
{
    return rc_crc32_update(0, buf, len);
}
