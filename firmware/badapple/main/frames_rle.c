#include "frames_rle.h"
#include <string.h>

static bool read_varint(const uint8_t **pp, const uint8_t *end, uint32_t *out)
{
    const uint8_t *p = *pp;
    uint32_t v = 0;
    int shift = 0;
    while (p < end) {
        uint8_t c = *p++;
        v |= (uint32_t)(c & 0x7f) << shift;
        if (!(c & 0x80)) {
            *pp = p;
            *out = v;
            return true;
        }
        shift += 7;
        if (shift > 28) break;
    }
    return false;
}

bool frames_decode(const uint8_t *p, const uint8_t *end, uint8_t *img,
                   uint16_t w, uint16_t h)
{
    const uint32_t row = (w + 7u) / 8u;
    const uint32_t bits = row * h * 8u;
    memset(img, 0, row * h);
    uint32_t pos = 0;
    int val = 0;
    while (pos < bits) {
        uint32_t run;
        if (!read_varint(&p, end, &run)) return false;
        if (run > bits - pos) run = bits - pos;
        if (val) {
            uint32_t done = 0;
            while (done < run && (pos & 7)) {
                img[pos >> 3] |= (uint8_t)(0x80 >> (pos & 7));
                pos++;
                done++;
            }
            uint32_t full = (run - done) / 8;
            if (full) {
                memset(img + (pos >> 3), 0xff, full);
                pos += full * 8;
                done += full * 8;
            }
            while (done < run) {
                img[pos >> 3] |= (uint8_t)(0x80 >> (pos & 7));
                pos++;
                done++;
            }
        } else {
            pos += run;
        }
        val ^= 1;
    }
    return true;
}
