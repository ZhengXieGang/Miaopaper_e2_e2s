#pragma once
/* text_internal.h — text 组件私有声明(非公共 API)。
 *
 * 只有 components/text 目录下的 .c 与 tools/test_text_host.c(PC 单测)包含本文件。
 * 公共头 include/text.h 已冻结,本文件只放包内实现细节(DESIGN §5、§8、§17)。
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "text.h"

/* ------------------------------------------------------------------ *
 * 返回码
 * ------------------------------------------------------------------ */
#define TEXT_OK           0
#define TEXT_ERR_PARAM    (-1)   /* 参数为空或非法 */
#define TEXT_ERR_IO       (-2)   /* text_source_t.read_at 失败 */
#define TEXT_ERR_UNSUP    (-3)   /* UTF-16 BOM 等不支持的格式 */
#define TEXT_ERR_TOO_BIG  (-4)   /* 超过 READER_UPLOAD_MAX_BYTES(32 MB) */

/* ------------------------------------------------------------------ *
 * 静态断言(DESIGN §5.4/§5.6)
 * ------------------------------------------------------------------ */
_Static_assert(READER_LINE_MAX_BYTES >= READER_CHARS_PER_LINE * 4,
               "line buffer too small for 32 chars x 4 bytes");
_Static_assert(READER_LINE_MAX_BYTES >= 64, "line buffer too small for GBK");
_Static_assert(READER_LINES_PER_PAGE == 5, "page layout assumes 5 lines");
_Static_assert(READER_CHAR_W_ASCII * READER_TAB_SPACES == 32, "tab must be 32 px");
_Static_assert(READER_TEXT_MARGIN_X * 2 + READER_TEXT_MAX_W <= READER_FB_W,
               "text margins exceed the frame width");
_Static_assert(READER_CONTENT_TOP + (READER_LINES_PER_PAGE - 1) * READER_LINE_H +
                       READER_ASCII_DY + 16 <= READER_FB_H,
               "the last text line overflows the frame height");

/* ------------------------------------------------------------------ *
 * 日志:固件用 esp_log,PC 单测用 stderr/stdout
 * ------------------------------------------------------------------ */
#ifdef TEXT_HOST_BUILD
#include <stdio.h>
#define TX_LOGE(tag, fmt, ...) fprintf(stderr, "E %s: " fmt "\n", tag, ##__VA_ARGS__)
#define TX_LOGW(tag, fmt, ...) fprintf(stderr, "W %s: " fmt "\n", tag, ##__VA_ARGS__)
#define TX_LOGI(tag, fmt, ...) fprintf(stdout, "I %s: " fmt "\n", tag, ##__VA_ARGS__)
#else
#include "esp_log.h"
#define TX_LOGE(tag, fmt, ...) ESP_LOGE(tag, fmt, ##__VA_ARGS__)
#define TX_LOGW(tag, fmt, ...) ESP_LOGW(tag, fmt, ##__VA_ARGS__)
#define TX_LOGI(tag, fmt, ...) ESP_LOGI(tag, fmt, ##__VA_ARGS__)
#endif

/* ------------------------------------------------------------------ *
 * 8x16 ASCII 字模
 * ------------------------------------------------------------------ */
/* 96 字形(0x20..0x7F),16 B/字,每字节一行,MSB 最左,bit=1 墨(font8x16.c)。 */
const uint8_t *font8x16_glyph(unsigned char ch);

/* ------------------------------------------------------------------ *
 * 内嵌资产(CMakeLists 按资产分别 EMBED_FILES 并分别定义宏)
 *
 * hz16_gb2312.bin 与 gb2312_map.bin 是两份独立资产,宏也独立:缺任何一份时
 * CMakeLists 会在 configure 阶段 FATAL_ERROR;这里做到"嵌入哪份才声明哪份",
 * 避免只留一份资产时仍声明另一份的 _binary_* 符号而链接失败。
 *
 * 许可(T8):assets/HZK16_87.bin 及由 --from-hzk16 生成的 hz16_gb2312.bin
 * 字模许可不明,只作内部/自用构建;对外发行前必须用
 * tools/gen_hz16.py --from-ttf 重新生成(docs/FONT.md §5、assets/NOTICE.md)。
 * 本代码不改变该许可策略,只在此处留下构建侧提示。
 * ------------------------------------------------------------------ */
#if defined(TEXT_ASSET_HZ16_EMBEDDED)
extern const uint8_t hz16_gb2312_start[] __asm__("_binary_hz16_gb2312_bin_start");
extern const uint8_t hz16_gb2312_end[] __asm__("_binary_hz16_gb2312_bin_end");
#endif
#if defined(TEXT_ASSET_MAP_EMBEDDED)
extern const uint8_t gb2312_map_start[] __asm__("_binary_gb2312_map_bin_start");
extern const uint8_t gb2312_map_end[] __asm__("_binary_gb2312_map_bin_end");
#endif

/* hz16_gb2312.bin:87 区 × 94 位 × 32 B = 261,696 B,16 行 × 2 B 大端,
 * MSB 最左,bit=1 墨(DESIGN §5.3)。 */
#define TEXT_HZ16_ROWS        87u
#define TEXT_HZ16_COLS        94u
#define TEXT_HZ16_SLOTS       (TEXT_HZ16_ROWS * TEXT_HZ16_COLS)   /* 8178 */
#define TEXT_HZ16_GLYPH_BYTES 32u
#define TEXT_HZ16_BYTES       (TEXT_HZ16_SLOTS * TEXT_HZ16_GLYPH_BYTES) /* 261696 */

/* gb2312_map.bin:12 B 头 + 7,445 条 (u16 Unicode, u16 区位槽) 按 Unicode 升序,
 * 共 29,792 B。头:0..3 "GBM1" | 4..5 u16 count | 6..7 u16 version=1 |
 * 8..11 u32 crc32(条目区)。只做 Unicode→区位 的正向查询(DESIGN §5.3)。 */
#define TEXT_MAP_MAGIC0  'G'
#define TEXT_MAP_MAGIC1  'B'
#define TEXT_MAP_MAGIC2  'M'
#define TEXT_MAP_MAGIC3  '1'
#define TEXT_MAP_VERSION 1u
#define TEXT_MAP_HEADER_BYTES 12u
#define TEXT_MAP_COUNT        7445u
#define TEXT_MAP_ENTRY_BYTES  4u
#define TEXT_MAP_BYTES (TEXT_MAP_HEADER_BYTES + TEXT_MAP_COUNT * TEXT_MAP_ENTRY_BYTES) /* 29792 */

/* GB2312 区位 → 字模槽号;hi/lo 是 GB2312 双字节(0xA1..0xF7 / 0xA1..0xFE)。 */
#define TEXT_HZ16_SLOT(hi, lo) \
    (((uint32_t)(hi) - 0xA1u) * TEXT_HZ16_COLS + ((uint32_t)(lo) - 0xA1u))
#define TEXT_HZ16_SLOT_OFFSET(slot) ((uint32_t)(slot) * TEXT_HZ16_GLYPH_BYTES)

#ifdef TEXT_HOST_BUILD
/* PC 单测:直接注入资产内容(固件走 EMBED_FILES 符号)。 */
void text_assets_set_host(const uint8_t *hz16, size_t hz16_len,
                          const uint8_t *map, size_t map_len);
#endif

/* ------------------------------------------------------------------ *
 * 字符宽度与解码(布局、字体、测量共用同一套规则,保证一致)
 * ------------------------------------------------------------------ */
/* 显示宽度(DESIGN §5.1/§5.4):Tab=32,其余控制字符=0,ASCII=8,非 ASCII=16
 * (含一切 4 字节字符与 Latin-1)。 */
static inline int tx_char_width(uint32_t cp)
{
    if (cp == (uint32_t)'\t') {
        return READER_TAB_SPACES * READER_CHAR_W_ASCII;
    }
    /* U+FEFF:UTF-8 BOM / 零宽不换行空格。DESIGN §5.4 字面写"其余 = 16 px",
     * 这里是有意例外:它是零宽格式字符,按 16 px 会画空心框并白占一格。
     * 与 Unicode 语义一致,保留零宽;建议 DESIGN §5.4 增补该例外(T7)。 */
    if (cp == 0xFEFFu) {
        return 0;
    }
    if (cp < 0x20u) {
        return 0;
    }
    return (cp < 0x80u) ? READER_CHAR_W_ASCII : READER_CHAR_W_CJK;
}

/* GBK 双字节字符在包内用这个哨兵码点表示(> U+10FFFF,不会与真实码点冲突)。 */
#define TX_GBK_CP(hi, lo) (0x110000u | ((uint32_t)(hi) << 8) | (uint32_t)(lo))
#define TX_IS_GBK_CP(cp)  ((cp) >= 0x110000u)
#define TX_GBK_HI(cp)     ((uint8_t)(((cp) >> 8) & 0xFFu))
#define TX_GBK_LO(cp)     ((uint8_t)((cp) & 0xFFu))

/* UTF-8 序列长度(严格):拒绝 0xC0/0xC1/0xF5..0xFF 起始。0 = 非法首字节。 */
static inline int tx_utf8_seq_len(uint8_t b)
{
    if (b < 0x80u) {
        return 1;
    }
    if (b >= 0xC2u && b <= 0xDFu) {
        return 2;
    }
    if (b >= 0xE0u && b <= 0xEFu) {
        return 3;
    }
    if (b >= 0xF0u && b <= 0xF4u) {
        return 4;
    }
    return 0;
}

/* 校验一个完整 UTF-8 序列并求出码点:拒绝过长编码、代理区、>U+10FFFF。 */
static inline bool tx_utf8_check(const uint8_t *p, int len, uint32_t *cp)
{
    if (!p || !cp || len < 1 || len > 4) {
        return false;
    }
    if (len == 1) {
        *cp = p[0];
        return true;
    }
    for (int i = 1; i < len; i++) {
        if ((p[i] & 0xC0u) != 0x80u) {
            return false;
        }
    }
    uint32_t v;
    switch (len) {
    case 2:
        v = ((uint32_t)(p[0] & 0x1Fu) << 6) | (uint32_t)(p[1] & 0x3Fu);
        break;
    case 3:
        v = ((uint32_t)(p[0] & 0x0Fu) << 12) | ((uint32_t)(p[1] & 0x3Fu) << 6) |
            (uint32_t)(p[2] & 0x3Fu);
        break;
    default:
        v = ((uint32_t)(p[0] & 0x07u) << 18) | ((uint32_t)(p[1] & 0x3Fu) << 12) |
            ((uint32_t)(p[2] & 0x3Fu) << 6) | (uint32_t)(p[3] & 0x3Fu);
        break;
    }
    if (len == 2 && v < 0x80u) {
        return false;
    }
    if (len == 3 && v < 0x800u) {
        return false;
    }
    if (len == 4 && (v < 0x10000u || v > 0x10FFFFu)) {
        return false;
    }
    if (v >= 0xD800u && v <= 0xDFFFu) {
        return false;
    }
    *cp = v;
    return true;
}

/* GBK 双字节规则(DESIGN §5.2):首 0x81..0xFE,次 0x40..0xFE 且 ≠0x7F。 */
static inline bool tx_gbk_lead_ok(uint8_t b)
{
    return b >= 0x81u && b <= 0xFEu;
}
static inline bool tx_gbk_trail_ok(uint8_t b)
{
    return b >= 0x40u && b <= 0xFEu && b != 0x7Fu;
}

/* ------------------------------------------------------------------ *
 * 断行与禁则(text_layout.c)
 * ------------------------------------------------------------------ */
typedef struct {
    uint64_t off;    /* 行首源偏移(字符边界) */
    uint64_t next;   /* 本行消费到的位置 = 下一行起点 */
    uint16_t len;    /* raw 中的字节数(源编码,≤ READER_LINE_MAX_BYTES) */
    uint16_t width;  /* 像素宽(≤ READER_TEXT_MAX_W,空行为 0) */
    uint8_t  chars;  /* 占位格数:ASCII/CJK=1,Tab=4 */
} tx_line_t;

/* 从 pos 开始排一行。返回 1=得到一行(可能是空行),0=已到 EOF,-1=I/O 错误。
 * raw 至少 READER_LINE_MAX_BYTES 字节;写入的是**源编码**字节(UTF-8 或 GBK),
 * 渲染时按 text_book_t.enc 解码。
 * 不变量:pos < size 时 next > pos(保证分页前进)。 */
int tx_layout_line(const text_book_t *b, uint64_t pos, tx_line_t *out, uint8_t *raw);

/* 同上,但允许用一段已读入内存的窗口作为预取(后向分页用,避免重复读 SD)。
 * seed[0] 对应源偏移 seed_off;窗口不够时仍会回源读。 */
int tx_layout_line_seeded(const text_book_t *b, uint64_t pos,
                          const uint8_t *seed, uint64_t seed_off, size_t seed_len,
                          tx_line_t *out, uint8_t *raw);

/* 把 pos 退到一个合法的字符边界(UTF-8 回退续字节,GBK 回退双字节次字节)。
 * 只在后向分页最后兜底时使用。 */
uint64_t tx_align_char_boundary(const text_book_t *b, uint64_t pos);

/* 禁则集合(DESIGN §5.4)。UTF-8 按码点;GBK 按双字节码(不含 ASCII,
 * ASCII 由调用方用 tx_is_closing/tx_is_opening 判断)。 */
bool tx_is_closing(uint32_t cp);
bool tx_is_opening(uint32_t cp);
bool tx_is_closing_gbk(uint8_t hi, uint8_t lo);
bool tx_is_opening_gbk(uint8_t hi, uint8_t lo);

/* ------------------------------------------------------------------ *
 * 绘制(text_font.c)
 * ------------------------------------------------------------------ */
/* 按编码绘制一行(不要求 NUL 结尾)。返回已占用的像素宽。
 * 解码失败的字节画 16 px 空心框;GB2312 覆盖不到的字同样画空心框。 */
int tx_draw_line(uint8_t *fb, int x, int y, const uint8_t *s, size_t len,
                 int max_w, int black, text_enc_t enc);

/* 字体资产是否可用(0 = 缺资产,CJK 全部画空心框)。 */
bool tx_font_assets_ok(void);
