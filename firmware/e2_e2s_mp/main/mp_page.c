/* mp_page.c — Miaopaper 分页算法的纯 C 移植（无 ESP-IDF / 无存储依赖）。
 *
 * 真源是 refs/Miaopaper/Firmware/src/ebook_layout.h（find_line_break_ascii /
 * find_line_break_gb2312 / eb_page_end / eb_find_prev_page_start）与
 * ebook.c 的 render_page_ascii()/render_page_gb2312()（ebook.c:776-851）。
 * 本文件不绘制、不刷新面板、不碰 BLE、不读写任何文件：书内容一律经
 * mp_book_ref_t.read 取得，存储策略由调用方（mp_app.c 的薄包装，最终落到
 * mp_store_read_book）决定。
 *
 * 页边界逐字节一致的三条规则（ebook.h:86-100、ebook_layout.h:22-58）：
 *   1. 每页最多 MP_READ_LINES(5) 行；每行只从源读 MP_READ_BUF(80) B；
 *      行宽上限 MP_MAX_LINE_W(246) px。
 *   2. ASCII：Dialog_plain_16 比例宽；'\n' 立即断行（含该字节），'\r' 跳过；
 *      越界字节（<0x20 或 >0x7E）**先替换成空格**（宽 6 px，且可作为回退点）；
 *      超宽退到最后一个空格之后，无空格硬断。
 *   3. GB2312：hi>=0xA1 && lo>=0xA1 为 2 字节全角；其余单字节也按 16 px
 *      （A3 区全角 ASCII 画法，ebook.c:827-841）。超宽即断，无回退。
 *
 * 一行"读完"的提前收尾条件与 M 相同：brk >= to_read && to_read < 80
 * （只有读到文件尾的短块才会收尾；正好 80 B 的整行继续下一行）。
 */
#include "mp_page.h"

#include <string.h>

#include "mp_font.h"

/* 需求 3 冻结的阅读版式：5 行 × 18 px。行距只属于渲染（mp_ui 用
 * MP_LINE_H 定位每个行顶 20+18i），分页只由"每页 5 行 + 每行 <=246 px /
 * <=80 B"决定（PORT_SPEC §4.4：页边界与像素行距无关）。这里把两个常量
 * 钉住，防止有人只改一边。 */
_Static_assert(MP_READ_LINES == 5, "阅读页必须是 5 行");
_Static_assert(MP_LINE_H == 18, "阅读行距必须是 18 px");

/* ------------------------------------------------------------------ *
 * ASCII 前进宽度
 * ------------------------------------------------------------------ *
 * 契约是 mp_gfx_adv(&Dialog_plain_16, c)（PORT_SPEC §4.8-3 / §10.4）。
 * 当前 mp_font.h 尚未声明该函数，而本文件只能写 mp_page.c，因此直接读
 * mp_font.h 已导出的 Dialog_plain_16 字形表，取值规则同 mp_gfx_adv：
 *   c 在表内 -> glyph[c-first].xAdvance
 *   否则     -> glyph[' '-first].xAdvance（本字体 = 6，与 M 一致）
 * 表比结构体声明的 [first,last] 少一项（见下），越界项按"否则"处理。
 * 注意**不能** include "fonts/font16.h"：那是非 static 的 const 定义，
 * 两个 TU 都包含会 multiple definition（PORT_SPEC §10.4 已实测）。
 * mp_font.h 日后补上 mp_gfx_adv() 时，本函数可等价替换为一行调用。 */
int mp_ascii_adv(uint8_t c)
{
    const GFXfont *f = &Dialog_plain_16;

    if (!f->glyph) {
        return MP_ASCII_MIN_W;
    }

    /* 注意：fonts/font16.h 的 Dialog_plain_16Glyphs 实际只有 94 项
     * （0x20..0x7D；最后一项是 '}'，bitmap 也恰好在那里结束），而结构体
     * 写的是 first=0x20 / last=0x7E —— '~' 没有字模。Miaopaper 的
     * find_line_break_ascii 不判界，对 '~' 会读到表外（未定义）。这里按
     * "表的实际项数 = last-first" 判界，'~' 因此退化成空格宽 6 px。 */
    int count = (int)f->last - (int)f->first;
    int idx = (int)c - (int)f->first;
    if (idx < 0 || idx >= count) {
        /* 越界字节按空格宽计（find_line_break_ascii 先替换成 ' ' 再取宽）。 */
        idx = (int)' ' - (int)f->first;
        if (idx < 0 || idx >= count) {
            return MP_ASCII_MIN_W;
        }
    }
    return f->glyph[idx].xAdvance;
}

/* 一段字节的"分页宽度"：与断行口径完全一致（不是绘制 ink 宽）。
 *   - ASCII：累加 mp_ascii_adv；'\n' 结束（0 宽），'\r' 跳过（0 宽）。
 *   - GB2312：每个字符 16 px；双字节与单字节同宽；'\n'/'\r' 不占宽。
 * mp_line_w(buf, brk, enc) 得到的就是本行实际占的 x 宽度（<=246）。 */
int mp_line_w(const uint8_t *buf, int len, uint8_t enc)
{
    if (!buf || len <= 0) {
        return 0;
    }

    int w = 0;
    if (enc == MP_ENC_GB2312) {
        int i = 0;
        while (i < len) {
            if (buf[i] == '\n') {
                break;
            }
            if (buf[i] == '\r') {
                i++;
                continue;
            }
            if (buf[i] >= 0xA1 && i + 1 < len && buf[i + 1] >= 0xA1) {
                i += 2;
            } else {
                i += 1;
            }
            w += MP_HZ_W;
        }
        return w;
    }

    for (int i = 0; i < len; i++) {
        if (buf[i] == '\n') {
            break;
        }
        if (buf[i] == '\r') {
            continue;
        }
        w += mp_ascii_adv(buf[i]);
    }
    return w;
}

/* find_line_break_ascii（ebook_layout.h:22-38）逐句移植。
 * 返回值保证 1..len（len<1 时返回 0）：首字符就超宽时 M 会返回 0，
 * 那会让分页循环原地打转；max_w=246 下不可达，这里按头文件契约兜底。 */
int mp_lb_ascii(const uint8_t *buf, int len, int max_w)
{
    if (!buf || len <= 0) {
        return 0;
    }

    int x = 0;
    int last_space = -1;

    for (int i = 0; i < len; i++) {
        if (buf[i] == '\n') {
            return i + 1;
        }
        if (buf[i] == '\r') {
            continue;
        }

        uint8_t c = buf[i];
        if (c < 0x20 || c > 0x7E) {
            c = ' ';              /* 与 M 相同：越界字节按空格参与排版 */
        }
        int cw = mp_ascii_adv(c);
        if (c == ' ') {
            last_space = i;       /* 越界字节替换后也是回退点 */
        }
        if (x + cw > max_w) {
            int brk = (last_space >= 0) ? last_space + 1 : i;
            return brk > 0 ? brk : 1;
        }
        x += cw;
    }
    return len;
}

/* find_line_break_gb2312（ebook_layout.h:40-58）逐句移植。 */
int mp_lb_gb2312(const uint8_t *buf, int len, int max_w)
{
    if (!buf || len <= 0) {
        return 0;
    }

    int x = 0;
    int i = 0;

    while (i < len) {
        if (buf[i] == '\n') {
            return i + 1;
        }
        if (buf[i] == '\r') {
            i++;
            continue;
        }

        int cb;
        if (buf[i] >= 0xA1 && i + 1 < len && buf[i + 1] >= 0xA1) {
            cb = 2;               /* 双字节全角 */
        } else {
            cb = 1;               /* 单字节也按 16 px（A3 全角 ASCII） */
        }
        if (x + MP_HZ_W > max_w) {
            return i > 0 ? i : 1;
        }
        x += MP_HZ_W;
        i += cb;
    }
    return len;
}

/* ------------------------------------------------------------------ *
 * 分页
 * ------------------------------------------------------------------ */

/* 一页的结束偏移。pos < 起点窗口上界，所有读都从 l->win 取。 */
typedef struct {
    const uint8_t *win;
    uint32_t win_base;
    uint32_t win_end;
    uint32_t book_len;
    uint8_t enc;
} mp_layout_t;

static uint32_t layout_page_end(const mp_layout_t *l, uint32_t pos)
{
    int line = 0;

    while (line < MP_READ_LINES && pos < l->book_len) {
        if (pos >= l->win_end) {
            break;                /* 窗口外：不会发生（调用方保证 pos < win_end） */
        }
        uint32_t to_read = l->book_len - pos;
        if (to_read > MP_READ_BUF) {
            to_read = MP_READ_BUF;
        }
        if (to_read > l->win_end - pos) {
            to_read = l->win_end - pos;
        }

        const uint8_t *chunk = l->win + (pos - l->win_base);
        int brk = (l->enc == MP_ENC_GB2312)
                      ? mp_lb_gb2312(chunk, (int)to_read, MP_MAX_LINE_W)
                      : mp_lb_ascii(chunk, (int)to_read, MP_MAX_LINE_W);
        if (brk < 1) {
            brk = 1;
        }
        if ((uint32_t)brk > to_read) {
            brk = (int)to_read;
        }

        pos += (uint32_t)brk;
        line++;
        if ((uint32_t)brk >= to_read && to_read < MP_READ_BUF) {
            break;                /* 读到书尾（短块），本页提前收尾 */
        }
    }
    return pos;
}

uint32_t mp_page_measure(const mp_book_ref_t *b, uint32_t pos, mp_page_t *out)
{
    if (!out) {
        return pos;
    }
    memset(out, 0, sizeof(*out));

    if (!b) {
        out->start = pos;
        out->next = pos;
        out->lines = 0;
        out->at_end = 0;
        return pos;
    }

    uint32_t len = b->len;
    uint32_t start = mp_page_clamp(pos, len);
    out->start = start;
    out->next = start;
    out->lines = 0;
    out->at_end = 0;

    /* 空书 / pos 已到书尾：lines=0，at_end=1（头文件冻结语义）。 */
    if (!b->read || start >= len) {
        out->at_end = (start >= len) ? 1 : 0;
        return start;
    }

    uint32_t p = start;
    int line = 0;
    uint8_t buf[MP_READ_BUF];

    while (line < MP_READ_LINES && p < len) {
        uint32_t to_read = len - p;
        if (to_read > MP_READ_BUF) {
            to_read = MP_READ_BUF;
        }

        /* 读失败（回调返回非 0；薄包装在短读/错误时返回负值）：
         * 不返回半页，lines=0 让调用方显示错误页，且不要原地重试。 */
        if (b->read(b->ctx, p, buf, (size_t)to_read) != 0) {
            out->lines = 0;
            out->next = start;
            out->at_end = 0;
            return start;
        }

        int brk = (b->enc == MP_ENC_GB2312)
                      ? mp_lb_gb2312(buf, (int)to_read, MP_MAX_LINE_W)
                      : mp_lb_ascii(buf, (int)to_read, MP_MAX_LINE_W);
        if (brk < 1) {
            brk = 1;              /* 防御：头部契约 1..len */
        }
        if ((uint32_t)brk > to_read) {
            brk = (int)to_read;
        }

        int copy = brk;
        if (copy > MP_READ_BUF) {
            copy = MP_READ_BUF;
        }
        out->line_off[line] = p;
        out->line_len[line] = (uint16_t)brk;   /* 含行尾 '\n' */
        out->line_w[line] = (uint16_t)mp_line_w(buf, copy, b->enc);
        memcpy(out->raw[line], buf, (size_t)copy);
        out->raw[line][copy] = '\0';

        p += (uint32_t)brk;
        line++;
        if ((uint32_t)brk >= to_read && to_read < MP_READ_BUF) {
            break;
        }
    }

    out->lines = (uint8_t)line;
    out->next = p;
    out->at_end = (p >= len) ? 1 : 0;
    return p;
}

/* eb_find_prev_page_start（ebook_layout.h:128-175）的移植。
 *
 * 窗口 [lo, S+min(80,len-S))：一页最多 5 行、一行最多 80 B，所以上一页起点
 * 不会早于 S-400；窗口再向后多带 80 B，因为"页尾恰好 == S"的判定需要读到
 * 起点在 S 之前、跨越 S 的那一行（溢出检查会看 S 处的字符）。
 *
 * 读失败时返回 S（不移动）：M 的 fill() 没有错误路径，这里"无上一页"是
 * 唯一安全的结果（mp_app 按 back >= S 直接忽略）。 */
uint32_t mp_page_prev_start(const mp_book_ref_t *b, uint32_t pos)
{
    if (!b) {
        return pos;
    }

    uint32_t len = b->len;
    uint32_t S = mp_page_clamp(pos, len);
    if (S == 0) {
        return 0;
    }
    if (!b->read) {
        return S;
    }

    uint32_t lo = (S > MP_PAGE_BACK_MAX) ? S - MP_PAGE_BACK_MAX : 0;
    uint32_t ahead = len - S;
    if (ahead > MP_READ_BUF) {
        ahead = MP_READ_BUF;
    }
    uint32_t win_end = S + ahead;
    uint32_t win_len = win_end - lo;
    if (win_len > MP_PAGE_WIN_BYTES) {
        return lo;                /* 防御：窗口装不下（不会发生） */
    }

    uint8_t win[MP_PAGE_WIN_BYTES];
    if (b->read(b->ctx, lo, win, (size_t)win_len) != 0) {
        return S;
    }

    mp_layout_t l;
    l.win = win;
    l.win_base = lo;
    l.win_end = win_end;
    l.book_len = len;
    l.enc = b->enc;

    if (lo == 0) {
        /* 窗口已到书头：从 0 正向走规范页链，返回最后一个起点 < S 的页。
         * S 落在页边界上时它就是上一页；S 在页中间时它是"包含 S-1 的页"。 */
        uint32_t p = 0;
        while (p < S) {
            uint32_t nxt = layout_page_end(&l, p);
            if (nxt >= S) {
                return p;
            }
            if (nxt <= p) {
                return p;         /* 防御：不会发生（brk>=1） */
            }
            p = nxt;
        }
        return p;
    }

    /* 从窗口起点**从小到大**找第一个"页尾恰好 == S"的候选。方向很重要：
     * 起点落在真实上一页内部的候选永远不可能在它之前结束，那是"掉头"页，
     * 会漏掉可见文字；更小的候选只会多吞 '\r' 或重排前缀，是真实上一页的
     * 超集。所以取最小的精确匹配。 */
    for (uint32_t c = lo; c < S; c++) {
        if (layout_page_end(&l, c) == S) {
            return c;
        }
    }

    /* 无精确匹配：S 在页中间。从窗口起点前进到"会跨过 S"的那一页。 */
    uint32_t p = lo;
    while (p < S) {
        uint32_t nxt = layout_page_end(&l, p);
        if (nxt >= S) {
            break;
        }
        if (nxt <= p) {
            break;
        }
        p = nxt;
    }
    return p;
}

/* ------------------------------------------------------------------ *
 * 进度 / 夹取
 * ------------------------------------------------------------------ */

uint8_t mp_page_percent(uint32_t pos, uint32_t len)
{
    if (len == 0) {
        return 0;
    }
    uint64_t v = (uint64_t)pos * 100u + (uint64_t)(len / 2u);   /* 四舍五入 */
    v /= (uint64_t)len;
    if (v > 100u) {
        v = 100u;
    }
    return (uint8_t)v;
}

uint32_t mp_page_clamp(uint32_t pos, uint32_t len)
{
    return (pos > len) ? len : pos;
}

/* ------------------------------------------------------------------ *
 * 本次会话的翻页历史（ebook.c:471-510 的 eb_page_hist）
 * ------------------------------------------------------------------ *
 * 只记录"从哪一页翻过来"；栈空时上一页由 mp_page_prev_start() 反推。
 * 深度 16，满了丢最旧一条。非线程安全：只允许 app 任务调用；打开书 /
 * 切书 / 解锁回阅读 / 关闭书时必须 mp_page_hist_reset()。 */
static uint32_t s_hist[MP_PAGE_HIST_DEPTH];
static uint8_t s_hist_len;

void mp_page_hist_reset(void)
{
    s_hist_len = 0;
}

void mp_page_hist_push(uint32_t pos)
{
    if (s_hist_len >= MP_PAGE_HIST_DEPTH) {
        for (int i = 1; i < MP_PAGE_HIST_DEPTH; i++) {
            s_hist[i - 1] = s_hist[i];
        }
        s_hist_len = MP_PAGE_HIST_DEPTH - 1;
    }
    s_hist[s_hist_len++] = pos;
}

uint8_t mp_page_hist_pop(uint32_t *pos)
{
    if (!pos || s_hist_len == 0) {
        return 0;
    }
    *pos = s_hist[--s_hist_len];
    return 1;
}

uint8_t mp_page_hist_len(void)
{
    return s_hist_len;
}
