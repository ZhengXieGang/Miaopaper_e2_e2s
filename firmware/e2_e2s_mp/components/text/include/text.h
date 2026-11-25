#pragma once
/* text.h — frozen text API (DESIGN.md §5, §8, §17.2)
 *
 * Encoding detection, line breaking, forward/backward pagination and
 * rendering. The text layer never touches SD directly: it reads through
 * text_source_t (storage_read_at implements it, DESIGN §8).
 */
#include <stddef.h>   /* size_t:text_source_t.read_at 的参数类型 */
#include <stdint.h>

#include "reader_cfg.h"

typedef enum {
    TEXT_ENC_AUTO = 0,
    TEXT_ENC_UTF8 = 1,
    TEXT_ENC_GBK = 2,
} text_enc_t;

typedef struct text_source {
    void *ctx;
    uint64_t size;
    int (*read_at)(void *ctx, uint64_t off, void *buf, size_t len);
} text_source_t;

typedef struct {
    uint64_t start, end;
    uint8_t line_count;
    struct { uint64_t off; uint16_t len, width; } line[READER_LINES_PER_PAGE];
    uint8_t raw[READER_LINES_PER_PAGE][READER_LINE_MAX_BYTES];
} text_page_t;

typedef struct {
    text_source_t src;
    text_enc_t enc;
    uint64_t size;
} text_book_t;

/* Scan the whole source once (32 KB static buffer, DESIGN §5.2), or trust a
 * persisted encoding when forced is TEXT_ENC_UTF8/TEXT_ENC_GBK.
 * forced 命中时不做任何 read_at(0 次 I/O);text 层不写 SD/NVS,0x26
 * SET_ENCODING 由 app 层把结果重新持久化(DESIGN §5.2/§6.2,T4)。 */
int text_open(text_book_t *b, const text_source_t *src, text_enc_t forced);

/* 分页无索引(DESIGN §5.5)。page_forward 返回 0 并填满 pg;page_backward 用
 * [S-1024, S+128) 窗口,不删除源字节,但允许页重叠/重复;找不到 end==S 或
 * 覆盖 S 的候选时退到 max(0,S-240) 兜底(T5)。 */
int text_page_forward(text_book_t *b, uint64_t pos, text_page_t *pg);
int text_page_backward(text_book_t *b, uint64_t pos, text_page_t *pg);

/* 把源偏移对齐到行首。行首约定与断行/分页一致:文件开头,或前一个字节是
 * '\n'(不处理 '\r\n' 的 '\r',也不做字符边界修正):
 *   off == 0        -> 0
 *   off 已是行首    -> off
 *   off >= size     -> size
 *   其它            -> off 之后(含 off)第一个 '\n' 的下一个字节;没有则 size
 * 读取失败时返回 size,由调用方按"文件尾"兜底。 */
uint64_t text_align_line(text_book_t *b, uint64_t off);

int text_percent(const text_book_t *b, uint64_t pos);

/* HZK16 字模(32 B,16 行 × 2 B,MSB 最左,bit=1 墨)。hi/lo 为 GB2312 双字节
 * (>0xA0);字模不存在或资产未加载时返回 NULL。只读,供 Miaopaper 兼容层
 * 直接按区位绘制(不需要经过 text_draw_line 的 8px ASCII 规则)。 */
const uint8_t *text_hz16_glyph(uint8_t hi, uint8_t lo);

/* 把 pg 画进 250x122 逻辑帧(1=白,0=黑)。pg->raw 保存的是**源编码**字节
 * (UTF-8 或 GBK),这里按 b->enc 解码后逐行绘制(包内 tx_draw_line);
 * 公共的 text_draw_line 只接受 UTF-8,不参与分页渲染。 */
void text_render_page(const text_book_t *b, const text_page_t *pg, uint8_t *fb);

/* Draw/measure a NUL-terminated UTF-8 line. Returns the used width in px.
 * 公共接口只接受 UTF-8;GBK 文本请先转成 UTF-8,或交给 text_render_page
 * 按 text_book_t.enc 解码绘制。 */
int text_draw_line(uint8_t *fb, int x, int y, const char *utf8, int max_w, int black);
int text_measure(const char *utf8);

const char *text_enc_name(text_enc_t enc);
