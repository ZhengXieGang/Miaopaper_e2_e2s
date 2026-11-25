/* text_font.c — 8x16 ASCII 与 16x16 GB2312 字模绘制(DESIGN §5.1/§5.3)。
 *
 * - ASCII(0x20..0x7E)来自 font8x16.c:8x16,垂直偏移 READER_ASCII_DY;
 * - 非 ASCII 固定 16 px 步进:UTF-8 用 gb2312_map.bin 做 Unicode→区位正向
 *   二分查找,GBK 直接按双字节码算区位槽;再取 hz16_gb2312.bin 的 32 B 字模;
 * - GB2312 覆盖不到的字(含 733 个未分配区位)与解码失败画 16 px 空心框;
 * - tab = 32 px 空白(不画点);其余 <0x20 不占宽。
 * 资产由 CMakeLists 以 EMBED_FILES 嵌入;缺失时全部 CJK 画空心框并降级显示,
 * 不 assert(PC 单测用 text_assets_set_host() 注入文件内容)。
 */
#include <stddef.h>
#include <stdint.h>

#include "text.h"

#include <string.h>

#include "gfx.h"
#include "rc_common.h"

#include "text_internal.h"

static const char *TAG = "text";

/* ------------------------------------------------------------------ *
 * 资产状态
 * ------------------------------------------------------------------ */
static const uint8_t *s_hz16;
static size_t s_hz16_len;
static const uint8_t *s_map;
static size_t s_map_len;
static uint16_t s_map_count;
static bool s_assets_done;
static bool s_map_ok;

/* 区位槽是否有字(GB2312 只分配了 8178 个槽里的 7445 个) */
static uint8_t s_slot_valid[(TEXT_HZ16_SLOTS + 7u) / 8u];
static bool s_slot_valid_done;

static inline uint16_t rd_u16(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static inline uint32_t rd_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static void slot_bits_build(void)
{
    if (s_slot_valid_done) {
        return;
    }
    s_slot_valid_done = true;
    memset(s_slot_valid, 0, sizeof(s_slot_valid));
    if (!s_map_ok) {
        return;
    }
    for (uint32_t i = 0; i < s_map_count; i++) {
        uint16_t slot = rd_u16(s_map + TEXT_MAP_HEADER_BYTES + (size_t)i * 4u + 2u);
        if (slot < TEXT_HZ16_SLOTS) {
            s_slot_valid[slot >> 3] |= (uint8_t)(1u << (slot & 7u));
        }
    }
}

static bool slot_is_valid(uint16_t slot)
{
    slot_bits_build();
    if (slot >= TEXT_HZ16_SLOTS) {
        return false;
    }
    return (s_slot_valid[slot >> 3] & (uint8_t)(1u << (slot & 7u))) != 0;
}

/* 校验 gb2312_map.bin 头(magic/版本/长度/CRC)。失败只降级,不 assert。 */
static void map_validate(void)
{
    s_map_ok = false;
    s_slot_valid_done = false;
    s_map_count = 0;

    if (!s_map || s_map_len < TEXT_MAP_HEADER_BYTES) {
        TX_LOGW(TAG, "gb2312_map.bin missing: CJK drawn as hollow boxes");
        return;
    }
    if (s_map[0] != TEXT_MAP_MAGIC0 || s_map[1] != TEXT_MAP_MAGIC1 ||
        s_map[2] != TEXT_MAP_MAGIC2 || s_map[3] != TEXT_MAP_MAGIC3) {
        TX_LOGE(TAG, "gb2312_map.bin bad magic");
        return;
    }
    uint16_t count = rd_u16(s_map + 4);
    uint16_t ver = rd_u16(s_map + 6);
    if ((size_t)count * TEXT_MAP_ENTRY_BYTES + TEXT_MAP_HEADER_BYTES != s_map_len) {
        TX_LOGE(TAG, "gb2312_map.bin size mismatch: %u entries, %u B",
                (unsigned)count, (unsigned)s_map_len);
        return;
    }
    uint32_t crc = rd_u32(s_map + 8);
    uint32_t calc = rc_crc32_ieee(s_map + TEXT_MAP_HEADER_BYTES,
                                  (size_t)count * TEXT_MAP_ENTRY_BYTES);
    if (crc != calc) {
        TX_LOGE(TAG, "gb2312_map.bin crc mismatch: %08lx != %08lx",
                (unsigned long)crc, (unsigned long)calc);
        return;
    }
    if (ver != TEXT_MAP_VERSION) {
        TX_LOGW(TAG, "gb2312_map.bin version %u (expected %u)",
                (unsigned)ver, (unsigned)TEXT_MAP_VERSION);
    }
    if (!s_hz16 || s_hz16_len < TEXT_HZ16_BYTES) {
        TX_LOGW(TAG, "hz16_gb2312.bin missing or short: %u B",
                (unsigned)s_hz16_len);
        return;
    }
    s_map_count = count;
    s_map_ok = true;
    TX_LOGI(TAG, "font assets ok: %u map entries, hz16 %u B",
            (unsigned)s_map_count, (unsigned)s_hz16_len);
}

static void ensure_assets(void)
{
    if (s_assets_done) {
        return;
    }
    s_assets_done = true;
    /* 两份资产各自独立:只取 CMake 实际嵌入的那份,缺失的一份降级为
     * "查不到字模"(CJK 画空心框),不会引用不存在的 _binary_* 符号。 */
#if defined(TEXT_ASSET_HZ16_EMBEDDED)
    s_hz16 = hz16_gb2312_start;
    s_hz16_len = (size_t)(hz16_gb2312_end - hz16_gb2312_start);
#else
    TX_LOGW(TAG, "hz16_gb2312.bin not embedded: run tools/gen_hz16.py");
#endif
#if defined(TEXT_ASSET_MAP_EMBEDDED)
    s_map = gb2312_map_start;
    s_map_len = (size_t)(gb2312_map_end - gb2312_map_start);
#else
    TX_LOGW(TAG, "gb2312_map.bin not embedded: run tools/gen_gb2312_map.py");
#endif
    map_validate();
}

#ifdef TEXT_HOST_BUILD
void text_assets_set_host(const uint8_t *hz16, size_t hz16_len,
                          const uint8_t *map, size_t map_len)
{
    s_hz16 = hz16;
    s_hz16_len = hz16_len;
    s_map = map;
    s_map_len = map_len;
    s_assets_done = true;
    map_validate();
}
#endif

bool tx_font_assets_ok(void)
{
    ensure_assets();
    return s_map_ok && s_hz16 && s_hz16_len >= TEXT_HZ16_BYTES;
}

/* Unicode → 区位槽(条目按 Unicode 升序,二分查找)。 */
static bool map_lookup(uint32_t cp, uint16_t *slot)
{
    if (!s_map_ok || cp > 0xFFFFu) {
        return false;
    }
    int lo = 0;
    int hi = (int)s_map_count - 1;
    while (lo <= hi) {
        int mid = lo + (hi - lo) / 2;
        const uint8_t *e = s_map + TEXT_MAP_HEADER_BYTES + (size_t)mid * 4u;
        uint16_t u = rd_u16(e);
        if (u == (uint16_t)cp) {
            uint16_t s = rd_u16(e + 2u);
            if (s >= TEXT_HZ16_SLOTS) {
                return false;
            }
            *slot = s;
            return true;
        }
        if (u < (uint16_t)cp) {
            lo = mid + 1;
        } else {
            hi = mid - 1;
        }
    }
    return false;
}

static const uint8_t *glyph_for_slot(uint16_t slot)
{
    if (!s_hz16 || slot >= TEXT_HZ16_SLOTS) {
        return NULL;
    }
    size_t off = (size_t)TEXT_HZ16_SLOT_OFFSET(slot);
    if (off + TEXT_HZ16_GLYPH_BYTES > s_hz16_len) {
        return NULL;
    }
    return s_hz16 + off;
}

/* ------------------------------------------------------------------ *
 * 绘制
 * ------------------------------------------------------------------ */
/* 1bpp MSB-first 位图贴到逻辑帧;bit=1 处按 black 置黑(1)或置白(0)。
 * 与 gfx_blit1 的区别:支持 black=0(反显菜单行用白字)。 */
static void blit_glyph(uint8_t *fb, int x, int y, int w, int h,
                       const uint8_t *bits, int stride, int black)
{
    if (!fb || !bits || w <= 0 || h <= 0 || stride <= 0) {
        return;
    }
    for (int j = 0; j < h; j++) {
        int yy = y + j;
        if (yy < 0 || yy >= READER_FB_H) {
            continue;
        }
        const uint8_t *row = bits + (size_t)j * (size_t)stride;
        for (int i = 0; i < w; i++) {
            if (!(row[i >> 3] & (uint8_t)(0x80u >> (i & 7)))) {
                continue;
            }
            int xx = x + i;
            if (xx < 0 || xx >= READER_FB_W) {
                continue;
            }
            uint8_t mask = (uint8_t)(0x80u >> (xx & 7));
            uint8_t *p = &fb[yy * READER_FB_ROW_BYTES + (xx >> 3)];
            if (black) {
                *p &= (uint8_t)~mask;
            } else {
                *p |= mask;
            }
        }
    }
}

int tx_draw_line(uint8_t *fb, int x, int y, const uint8_t *s, size_t len,
                 int max_w, int black, text_enc_t enc)
{
    if (!fb || !s) {
        return 0;
    }
    ensure_assets();

    const bool gbk = (enc == TEXT_ENC_GBK);
    int cx = x;
    int limit = (max_w > 0) ? x + max_w : READER_FB_W;
    size_t i = 0;

    while (i < len) {
        uint32_t cp = 0xFFFD;
        int nb = 1;
        bool pair = false;

        if (gbk) {
            if (s[i] < 0x80u) {
                cp = s[i];
                nb = 1;
            } else if (i + 1u < len && tx_gbk_lead_ok(s[i]) && tx_gbk_trail_ok(s[i + 1u])) {
                cp = TX_GBK_CP(s[i], s[i + 1u]);
                nb = 2;
                pair = true;
            }
        } else {
            int l = tx_utf8_seq_len(s[i]);
            if (l > 0 && i + (size_t)l <= len && tx_utf8_check(s + i, l, &cp)) {
                nb = l;
            }
        }

        int w = tx_char_width(cp);
        if (w == 0) {                       /* 其余控制字符:不占宽 */
            i += (size_t)nb;
            continue;
        }
        if (cx + w > limit) {
            break;
        }
        if (cp == (uint32_t)'\t') {         /* tab = 4 空格宽,不画点 */
            cx += w;
            i += (size_t)nb;
            continue;
        }

        if (cp < 0x80u) {
            const uint8_t *g = font8x16_glyph((unsigned char)cp);
            blit_glyph(fb, cx, y + READER_ASCII_DY, READER_CHAR_W_ASCII, 16, g, 1, black);
        } else {
            const uint8_t *g = NULL;
            if (pair) {
                uint8_t hi = TX_GBK_HI(cp);
                uint8_t lo = TX_GBK_LO(cp);
                if (hi >= 0xA1u && hi <= 0xF7u && lo >= 0xA1u && lo <= 0xFEu) {
                    uint16_t slot = (uint16_t)TEXT_HZ16_SLOT(hi, lo);
                    if (slot_is_valid(slot)) {
                        g = glyph_for_slot(slot);
                    }
                }
            } else {
                uint16_t slot;
                if (map_lookup(cp, &slot)) {
                    g = glyph_for_slot(slot);
                }
            }
            if (g) {
                /* 16 行 × 2 B 大端,MSB 最左,bit=1 墨 */
                blit_glyph(fb, cx, y, READER_CHAR_W_CJK, 16, g, 2, black);
            } else {
                /* GB2312 覆盖不到 / 解码失败:16 px 空心框(DESIGN §5.2) */
                gfx_frame(fb, cx, y, READER_CHAR_W_CJK, READER_CHAR_W_CJK, black);
            }
        }
        cx += w;
        i += (size_t)nb;
    }
    return cx - x;
}

int text_draw_line(uint8_t *fb, int x, int y, const char *utf8, int max_w, int black)
{
    if (!fb || !utf8) {
        return 0;
    }
    return tx_draw_line(fb, x, y, (const uint8_t *)utf8, strlen(utf8), max_w,
                        black, TEXT_ENC_UTF8);
}

int text_measure(const char *utf8)
{
    if (!utf8) {
        return 0;
    }
    size_t len = strlen(utf8);
    size_t i = 0;
    int w = 0;
    while (i < len) {
        uint32_t cp = 0xFFFD;
        int l = tx_utf8_seq_len((uint8_t)utf8[i]);
        if (l > 0 && i + (size_t)l <= len &&
            tx_utf8_check((const uint8_t *)utf8 + i, l, &cp)) {
            i += (size_t)l;
        } else {
            cp = 0xFFFD;
            i += 1u;
        }
        w += tx_char_width(cp);
    }
    return w;
}

/* Miaopaper 兼容层:按 GB2312 区位直接取 32 B 字模。 */
const uint8_t *text_hz16_glyph(uint8_t hi, uint8_t lo)
{
    ensure_assets();
    if (hi < 0xA1u || hi > 0xF7u || lo < 0xA1u || lo > 0xFEu) {
        return NULL;
    }
    uint16_t slot = (uint16_t)(((uint16_t)hi - 0xA1u) * TEXT_HZ16_COLS +
                               ((uint16_t)lo - 0xA1u));
    if (!slot_is_valid(slot)) {
        return NULL;
    }
    return glyph_for_slot(slot);
}
