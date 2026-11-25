#pragma once
/* mp_page.h — 分页契约（纯 C，无 ESP-IDF 依赖，主机模拟器可直接编译）。
 *
 * 目标：页边界与 Miaopaper `Firmware/src/ebook_layout.h` + `ebook.c` 的
 * `render_page_ascii()/render_page_gb2312()` **逐字节一致**。协议侧（BLE 0x10 的
 * total/enc、0x11 的偏移）和"上一页"都依赖它，不能"重新设计"。
 *
 * 三条不可漂移的规则（对应 ebook.h:86-100、ebook_layout.h:22-58）：
 *   1. 每页固定 5 行；每行最多从源文件读 80 B；行宽上限 246 px。
 *   2. ASCII：比例宽，'\n' 立即断行，'\r' 跳过；超宽退到**最后一个空格之后**，
 *      无空格则硬断。ASCII 模式按 Dialog_plain_16 的 xAdvance 累加；<0x20 或
 *      >0x7E 的字节按空格宽（6 px）计（与 find_line_break_ascii 的替换一致）。
 *   3. GB2312：hi>=0xA1 && lo>=0xA1 视为 2 字节全角；其余单字节**也按 16 px**
 *      计（ebook.c:827-841 的 A3 区全角画法）。超宽即断，无回退。
 *
 * 渲染由 mp_ui.c 负责；本模块只测量 + 给出每行原始字节。mp_page_measure() 返回
 * 的 `raw[]` 已是 NUL 结尾的行内容，可直接交给 mp_text_draw()。
 */
#include <stddef.h>
#include <stdint.h>

/* ------------------------------------------------------------------ *
 * 常量（与 ebook.h 对齐；改动即破坏与 Miaopaper 主机的兼容）
 * ------------------------------------------------------------------ */
/* 编码值必须与 mp_store.h 的 MP_ENC_ASCII/MP_ENC_GB2312 相同。
 * 同名同值宏重复定义在 C 里合法；mp_app.c 另有一条 _Static_assert 兜底。 */
#define MP_ENC_ASCII 0
#define MP_ENC_GB2312 1

#define MP_READ_LINES 5          /* EB_LINES_PER_PAGE */
#define MP_READ_BUF 80           /* EB_READ_BUF_SIZE：单行读取上限 */
#define MP_MAX_LINE_W 246        /* EB_MAX_LINE_W = 250 - 2*2 */
#define MP_HZ_W 16               /* EB_HZ_CHAR_W */
#define MP_HZ_H 16               /* EB_HZ_CHAR_H */
#define MP_ASCII_MIN_W 6         /* Dialog_plain_16 的空格宽；越界字节按此计 */
#define MP_LINE_H 18             /* 阅读区行距（本工程固定 18，见 PORT_SPEC §4.4） */

/* 一页最多消耗 5 × 80 B，所以上一页的起点一定在 [S-400, S+80) 内。 */
#define MP_PAGE_BACK_MAX (MP_READ_LINES * MP_READ_BUF)          /* 400 */
#define MP_PAGE_WIN_BYTES (MP_PAGE_BACK_MAX + MP_READ_BUF)      /* 480 */
#define MP_PAGE_HIST_DEPTH 16    /* ebook.c:471 的 eb_page_hist 深度 */
#define MP_LINE_BYTES (MP_READ_BUF + 1)  /* 行原始字节 + NUL */
#define MP_TITLE_MAX 19          /* 与 mp_store.h 一致：目录 title 最多 19 B */

/* ------------------------------------------------------------------ *
 * 行宽 / 断行原语（公开给主机测试，mp_page.c 实现）
 * ------------------------------------------------------------------ */
/* 单字节 ASCII 前进宽度，必须与 Miaopaper 的 find_line_break_ascii 完全一致
 * （ebook_layout.h:29-30：越界字节先替换成空格再取宽度）：
 *   (c < 0x20 || c > 0x7E) ? MP_ASCII_MIN_W(=6, Dialog_plain_16 的空格宽)
 *                          : mp_gfx_adv(&Dialog_plain_16, c)
 * 实现即这一行，不许写成"越界返回 8"——那会让含 UTF-8 字节的 ASCII 书
 * 每字节多 2 px，页边界与主机不一致（PORT_SPEC §4.8）。
 * 注：若 mp_font.h 尚未导出 mp_gfx_adv()，mp_page.c 直接读
 * Dialog_plain_16.glyph[c-first].xAdvance（越界取空格字形），语义相同。 */
int mp_ascii_adv(uint8_t c);

/* 一段字节在一行里占的像素宽（不做断行）。enc 取 MP_ENC_*。 */
int mp_line_w(const uint8_t *buf, int len, uint8_t enc);

/* 断行：返回本行应消耗的字节数（1..len）。语义与
 * find_line_break_ascii()/find_line_break_gb2312() 完全一致：
 *  - '\n' 立即返回（含该字节）；
 *  - '\r' 跳过；
 *  - 超宽返回 break 点；ASCII 优先回退到最后一个空格之后；
 *  - 一行读完返回 len。 */
int mp_lb_ascii(const uint8_t *buf, int len, int max_w);
int mp_lb_gb2312(const uint8_t *buf, int len, int max_w);

/* ------------------------------------------------------------------ *
 * 分页
 * ------------------------------------------------------------------ */
/* 读书回调：把 [off, off+len) 填进 buf。成功返回 0。
 * 固件侧传 mp_store_read_book 的薄包装；主机侧可以读内存。 */
typedef int (*mp_read_fn)(void *ctx, uint32_t off, void *buf, size_t len);

typedef struct {
    mp_read_fn read;
    void *ctx;
    uint32_t len;   /* 全书字节数（mp_store 目录项的 len） */
    uint8_t enc;    /* MP_ENC_ASCII / MP_ENC_GB2312 */
} mp_book_ref_t;

typedef struct {
    uint32_t start;   /* 本页首字节偏移 */
    uint32_t next;    /* 下一页首字节；>= len 表示本页是最后一页 */
    uint8_t lines;    /* 0..MP_READ_LINES（0 = 读失败或空书，调用方显示错误页） */
    uint8_t at_end;   /* next >= len */
    uint32_t line_off[MP_READ_LINES];  /* 每行在书内的起始偏移 */
    uint16_t line_len[MP_READ_LINES];  /* 每行消耗的字节数（含行尾 '\n'） */
    uint16_t line_w[MP_READ_LINES];    /* 每行像素宽（<= 246） */
    uint8_t raw[MP_READ_LINES][MP_LINE_BYTES];  /* NUL 结尾，可直接绘制 */
} mp_page_t;

/* 测量从 pos 开始的一页。返回下一页起点（== out->next）；读失败时返回 pos
 * 并把 out->lines 置 0（调用方据此显示错误页，不要死循环）。
 * pos 会被夹到 [0, len]；pos >= len 时 lines=0, at_end=1。
 * **不计算上一页**：需要上一页时由调用方显式调 mp_page_prev_start()（或先弹
 * 历史栈）。这样翻页只花 1 次窗口读，不会每页多 480 B。 */
uint32_t mp_page_measure(const mp_book_ref_t *b, uint32_t pos, mp_page_t *out);

/* 布局反推上一页起点（ebook_layout.h:128-175 的移植）。唯一入口：
 * 调用方先试 mp_page_hist_pop()，栈空再调本函数。
 *   窗口 lo = max(0, S-400)、win_end = S + min(80, len-S)；
 *   lo==0 时从 0 正向走页链，返回最后一个起点 < S 的页；
 *   否则在 [lo, S) 里从小到大找第一个 eb_page_end(c)==S 的候选；
 *   无精确匹配则退化为"包含字节 S-1 的页"。
 * S==0 返回 0；S>=len 时按 len 处理。 */
uint32_t mp_page_prev_start(const mp_book_ref_t *b, uint32_t pos);

/* 阅读进度百分比 = pos*100/len（四舍五入到整数、夹在 0..100；len==0 返回 0）。
 * 用 64 位中间量，8 MB 书不会溢出。 */
uint8_t mp_page_percent(uint32_t pos, uint32_t len);

/* pos 夹到 [0, len]。 */
uint32_t mp_page_clamp(uint32_t pos, uint32_t len);

/* ------------------------------------------------------------------ *
 * 本次会话的翻页历史（ebook.c:471-510）
 * ------------------------------------------------------------------ *
 * 只保存"从哪一页翻过来的"；栈空时上一页走 mp_page_prev_start()。
 * 深度 16，满了丢最旧的一条。非线程安全：只允许 app 任务调用。
 * 打开书 / 切书 / 解锁回阅读时必须 mp_page_hist_reset()。 */
void mp_page_hist_reset(void);
void mp_page_hist_push(uint32_t pos);
uint8_t mp_page_hist_pop(uint32_t *pos);   /* 1 = 弹出成功 */
uint8_t mp_page_hist_len(void);
