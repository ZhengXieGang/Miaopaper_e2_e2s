/* rc_util.c — FNV-1a, clamping, filename sanitising, strict UTF-8, clock.
 * Skeleton owner. Pure except rc_now_ms (esp_timer, monotonic).
 */
#include "rc_common.h"

#include <string.h>

#include "esp_timer.h"

uint32_t rc_fnv1a32(const char *s)
{
    uint32_t h = 2166136261u;
    if (!s) {
        return h;
    }
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        h ^= *p;
        h *= 16777619u;
    }
    return h;
}

int rc_clamp(int v, int lo, int hi)
{
    if (v < lo) {
        return lo;
    }
    if (v > hi) {
        return hi;
    }
    return v;
}

static bool is_forbidden(uint8_t c)
{
    if (c < 0x20 || c == 0x7F) {
        return true;
    }
    return strchr("/\\:*?\"<>|", (int)c) != NULL;
}

int rc_sanitize_filename(const char *in, char *out, size_t out_size)
{
    if (!in || !out || out_size < 2) {
        return -1;
    }
    size_t o = 0;
    for (size_t i = 0; in[i]; i++) {
        uint8_t c = (uint8_t)in[i];
        if (c == '.' && in[i + 1] == '.') {
            i++;                 /* drop the whole ".." pair */
            continue;
        }
        if (is_forbidden(c)) {
            continue;
        }
        if (o + 1 >= out_size) {
            return -1;
        }
        out[o++] = (char)c;
    }
    while (o > 0 && (out[o - 1] == ' ' || out[o - 1] == '.')) {
        o--;
    }
    size_t start = 0;
    while (start < o && (out[start] == ' ' || out[start] == '.')) {
        start++;
    }
    if (start > 0 && o > start) {
        memmove(out, out + start, o - start);
        o -= start;
    } else if (start >= o) {
        o = 0;
    }
    out[o] = '\0';
    return o ? (int)o : -1;
}

bool rc_utf8_decode(const uint8_t *s, size_t len, size_t *i, uint32_t *cp)
{
    if (!s || !i || !cp || *i >= len) {
        return false;
    }
    size_t k = *i;
    uint8_t c = s[k];
    if (c < 0x80) {
        *cp = c;
        *i = k + 1;
        return true;
    }

    int n;
    uint32_t v;
    if (c >= 0xC2 && c <= 0xDF) {
        n = 1;
        v = c & 0x1F;
    } else if (c >= 0xE0 && c <= 0xEF) {
        n = 2;
        v = c & 0x0F;
    } else if (c >= 0xF0 && c <= 0xF4) {
        n = 3;
        v = c & 0x07;
    } else {
        *cp = 0xFFFD;
        *i = k + 1;
        return false;
    }

    if (k + (size_t)n >= len) {
        *cp = 0xFFFD;
        *i = k + 1;
        return false;
    }
    for (int j = 1; j <= n; j++) {
        uint8_t cc = s[k + (size_t)j];
        if ((cc & 0xC0) != 0x80) {
            *cp = 0xFFFD;
            *i = k + 1;
            return false;
        }
        v = (v << 6) | (cc & 0x3F);
    }

    if ((n == 1 && v < 0x80) || (n == 2 && v < 0x800) || (n == 3 && v < 0x10000)) {
        *cp = 0xFFFD;            /* overlong */
        *i = k + 1;
        return false;
    }
    if ((v >= 0xD800 && v <= 0xDFFF) || v > 0x10FFFF) {
        *cp = 0xFFFD;
        *i = k + 1;
        return false;
    }
    *cp = v;
    *i = k + (size_t)n + 1;
    return true;
}

int rc_utf8_encode(uint32_t cp, char out[4])
{
    if (!out || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) {
        return 0;
    }
    if (cp < 0x80) {
        out[0] = (char)cp;
        return 1;
    }
    if (cp < 0x800) {
        out[0] = (char)(0xC0 | (cp >> 6));
        out[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    }
    if (cp < 0x10000) {
        out[0] = (char)(0xE0 | (cp >> 12));
        out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    }
    out[0] = (char)(0xF0 | (cp >> 18));
    out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
    out[3] = (char)(0x80 | (cp & 0x3F));
    return 4;
}

uint64_t rc_now_ms(void)
{
    return (uint64_t)(esp_timer_get_time() / 1000);
}
