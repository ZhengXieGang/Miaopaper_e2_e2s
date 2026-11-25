#pragma once
/* rc_common.h — small shared utilities (DESIGN.md §17.2)
 *
 * Implementation lives in components/common/crc32.c and rc_util.c.
 * These helpers are pure (no I/O, no globals) so the host-side text tests in
 * tools/ can link the same object files.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Reflected CRC-32 (IEEE 802.3, poly 0xEDB88320, init/xorout 0xFFFFFFFF).
 * rc_crc32_ieee("123456789", 9) == 0xCBF43926. */
uint32_t rc_crc32_update(uint32_t crc, const void *buf, size_t len);
uint32_t rc_crc32_ieee(const void *buf, size_t len);

/* FNV-1a 32-bit; used for the 8-hex-digit book id (DESIGN §6.1). */
uint32_t rc_fnv1a32(const char *s);

int rc_clamp(int v, int lo, int hi);

/* Strip '/', '\', ':', '*', '?', '"', '<', '>', '|', control bytes and any
 * ".." sequence. Returns the resulting length (0..out_size-2) or -1 when the
 * result would be empty or not fit. Always NUL-terminates on success. */
int rc_sanitize_filename(const char *in, char *out, size_t out_size);

/* Strict UTF-8 decoder. Decodes the character at s[*i]:
 *  - on success advances *i past the sequence and returns true;
 *  - on an invalid/overlong/surrogate/out-of-range/truncated sequence
 *    advances *i by exactly 1, sets *cp = 0xFFFD and returns false
 *    (DESIGN §5.2: never advance by 0). */
bool rc_utf8_decode(const uint8_t *s, size_t len, size_t *i, uint32_t *cp);

/* Encode one code point. Returns 1..4, or 0 when cp is a surrogate or
 * greater than U+10FFFF. */
int rc_utf8_encode(uint32_t cp, char out[4]);

/* Milliseconds since boot (monotonic, esp_timer based). */
uint64_t rc_now_ms(void);
