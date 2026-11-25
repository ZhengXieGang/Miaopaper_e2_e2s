/* text_layout.c — 断行、禁则与行对齐(DESIGN §5.1/§5.4/§5.5)。
 *
 * 规则:
 *   - 逐字符解码累加宽度;'\n' 断行;'\r' 跳过;其余 <0x20 跳过;'\t'=32 px;
 *   - 超过 READER_TEXT_MAX_W(246 px)或 32 字时在该字符前断行;本行 0 字时
 *     强制吃 1 字防死循环;
 *   - 禁则两套小集合(UTF-8 按码点、GBK 按双字节码,不需要区位→Unicode 反查):
 *     溢出字符属于收尾集合且本行 ≥2 字 → 本行最后一字退到下一行(只退一次);
 *     本行最后一字属于起始集合 → 同样退;
 *   - 解码失败:替换符(渲染为空心框)+ 前进 1 B;每行 next 严格大于 pos;
 *   - 行内原始字节 ≤ READER_LINE_MAX_BYTES(128 B,静态断言在 text_internal.h)。
 *
 * 本文件写进 text_page_t.raw 的是**源编码**字节(UTF-8 或 GBK),渲染时由
 * text_font.c 按 text_book_t.enc 解码,避免 GBK→Unicode 的反查表。
 */
#include <stddef.h>
#include <stdint.h>

#include "text.h"

#include <string.h>

#include "text_internal.h"

static const char *TAG = "text";

/* ------------------------------------------------------------------ *
 * 禁则集合(DESIGN §5.4)
 * ------------------------------------------------------------------ */
/* 收尾(不能出现在行首,可以挤在行尾):
 * 。，、；：！？）》」』】〉〕〗”’…― 与 ASCII , . ; : ! ? ) ] } */
static const uint32_t k_closing[] = {
    0x3002u, /* 。 */
    0xFF0Cu, /* ， */
    0x3001u, /* 、 */
    0xFF1Bu, /* ； */
    0xFF1Au, /* ： */
    0xFF01u, /* ！ */
    0xFF1Fu, /* ？ */
    0xFF09u, /* ) */
    0x300Bu, /* 》 */
    0x300Du, /* 」 */
    0x300Fu, /* 』 */
    0x3011u, /* 】 */
    0x3009u, /* 〉 */
    0x3015u, /* 〕 */
    0x3017u, /* 〗 */
    0x201Du, /* ” */
    0x2019u, /* ’ */
    0x2026u, /* … */
    0x2015u, /* ― */
    0x002Cu, /* , */
    0x002Eu, /* . */
    0x003Bu, /* ; */
    0x003Au, /* : */
    0x0021u, /* ! */
    0x003Fu, /* ? */
    0x0029u, /* ) */
    0x005Du, /* ] */
    0x007Du, /* } */
};

/* 起始(不能出现在行尾,可以挤在行首):
 * （《「『【〈〔〖“‘ 与 ASCII ( [ { */
static const uint32_t k_opening[] = {
    0xFF08u, /* ( */
    0x300Au, /* 《 */
    0x300Cu, /* 「 */
    0x300Eu, /* 『 */
    0x3010u, /* 【 */
    0x3008u, /* 〈 */
    0x3014u, /* 〔 */
    0x3016u, /* 〖 */
    0x201Cu, /* “ */
    0x2018u, /* ‘ */
    0x0028u, /* ( */
    0x005Bu, /* [ */
    0x007Bu, /* { */
};

/* GBK 收尾:按 §13.1 实测的区位码(DESIGN §5.4 与 §14.1)。
 * A1A2、A1A3、A1AD、A1AA、A1AF、A1B1、A1B3、A1B5、A1B7、A1B9、A1BB、A1BD、
 * A1BF、A3A1、A3AC、A3BA、A3BB、A3BF */
static const uint16_t k_closing_gbk[] = {
    0xA1A2u, 0xA1A3u, 0xA1ADu, 0xA1AAu, 0xA1AFu, 0xA1B1u,
    0xA1B3u, 0xA1B5u, 0xA1B7u, 0xA1B9u, 0xA1BBu, 0xA1BDu,
    0xA1BFu, 0xA3A1u, 0xA3ACu, 0xA3BAu, 0xA3BBu, 0xA3BFu,
};

/* GBK 起始:A1AE、A1B0、A1B2、A1B4、A1B6、A1B8、A1BA、A1BC、A1BE、A3A8 */
static const uint16_t k_opening_gbk[] = {
    0xA1AEu, 0xA1B0u, 0xA1B2u, 0xA1B4u, 0xA1B6u,
    0xA1B8u, 0xA1BAu, 0xA1BCu, 0xA1BEu, 0xA3A8u,
};

#define TX_ARRAY_LEN(a) (sizeof(a) / sizeof((a)[0]))

bool tx_is_closing(uint32_t cp)
{
    for (size_t i = 0; i < TX_ARRAY_LEN(k_closing); i++) {
        if (k_closing[i] == cp) {
            return true;
        }
    }
    return false;
}

bool tx_is_opening(uint32_t cp)
{
    for (size_t i = 0; i < TX_ARRAY_LEN(k_opening); i++) {
        if (k_opening[i] == cp) {
            return true;
        }
    }
    return false;
}

bool tx_is_closing_gbk(uint8_t hi, uint8_t lo)
{
    uint16_t code = (uint16_t)(((uint16_t)hi << 8) | lo);
    for (size_t i = 0; i < TX_ARRAY_LEN(k_closing_gbk); i++) {
        if (k_closing_gbk[i] == code) {
            return true;
        }
    }
    return false;
}

bool tx_is_opening_gbk(uint8_t hi, uint8_t lo)
{
    uint16_t code = (uint16_t)(((uint16_t)hi << 8) | lo);
    for (size_t i = 0; i < TX_ARRAY_LEN(k_opening_gbk); i++) {
        if (k_opening_gbk[i] == code) {
            return true;
        }
    }
    return false;
}

/* ------------------------------------------------------------------ *
 * 带窗口的源读取器
 *
 * 一次 read_at 取 320 B;一行最多 128 B 原始字节(最多 4 B/字),窗口足够
 * 覆盖一整行。窗口不够时(例如被大量跳过的控制字符)继续预读。
 * ------------------------------------------------------------------ */
#define TX_WIN_BYTES 320u

typedef struct {
    const text_book_t *b;
    const uint8_t *seed;    /* 已读入内存的窗口(可为 NULL) */
    uint64_t seed_off;      /* seed[0] 对应的源偏移 */
    uint32_t seed_len;
    uint64_t base;          /* buf[0] 对应的源偏移 */
    uint32_t len;           /* buf 中有效字节数 */
    uint32_t idx;           /* 游标(base+idx 即当前源偏移) */
    bool     err;
    uint8_t  buf[TX_WIN_BYTES];
} tx_rd_t;

static void rd_init(tx_rd_t *r, const text_book_t *b, uint64_t pos,
                    const uint8_t *seed, uint64_t seed_off, size_t seed_len)
{
    r->b = b;
    r->base = pos;
    r->len = 0;
    r->idx = 0;
    r->err = false;
    r->seed_off = seed_off;
    r->seed_len = (seed && seed_len > 0) ? (uint32_t)seed_len : 0;
    r->seed = (r->seed_len > 0) ? seed : NULL;
    if (r->seed && pos >= seed_off && pos - seed_off < seed_len) {
        size_t skip = (size_t)(pos - seed_off);
        size_t n = seed_len - skip;
        if (n > TX_WIN_BYTES) {
            n = TX_WIN_BYTES;
        }
        memcpy(r->buf, seed + skip, n);
        r->len = (uint32_t)n;
    }
}

/* 保证从游标起至少有 need 字节(除非到 EOF)。I/O 错误置 err。 */
static bool rd_ensure(tx_rd_t *r, size_t need)
{
    while ((size_t)(r->len - r->idx) < need) {
        if (r->err) {
            return false;
        }
        if (r->base + r->len >= r->b->size) {
            return false;   /* EOF */
        }
        uint32_t keep = r->len - r->idx;
        if (r->idx > 0) {
            memmove(r->buf, r->buf + r->idx, keep);
            r->base += r->idx;
            r->len = keep;
            r->idx = 0;
        }
        size_t want = TX_WIN_BYTES - r->len;
        uint64_t off = r->base + r->len;
        if (r->b->size - off < (uint64_t)want) {
            want = (size_t)(r->b->size - off);
        }
        if (want == 0) {
            return false;
        }
        /* 预读窗口覆盖到的部分直接从 seed 拷,避免重复读 SD(后向分页用) */
        if (r->seed && off >= r->seed_off && off - r->seed_off < r->seed_len) {
            size_t so = (size_t)(off - r->seed_off);
            size_t n = r->seed_len - so;
            if (n > want) {
                n = want;
            }
            memcpy(r->buf + r->len, r->seed + so, n);
            r->len += (uint32_t)n;
            off += (uint64_t)n;
            want -= n;
            if (want == 0) {
                continue;
            }
        }
        if (r->b->size - off < (uint64_t)want) {
            want = (size_t)(r->b->size - off);
        }
        if (want == 0) {
            continue;
        }
        if (r->b->src.read_at(r->b->src.ctx, off, r->buf + r->len, want) != 0) {
            r->err = true;
            return false;
        }
        r->len += (uint32_t)want;
    }
    return true;
}

/* ------------------------------------------------------------------ *
 * 单行排版
 * ------------------------------------------------------------------ */
static int layout_impl(const text_book_t *b, uint64_t pos,
                       const uint8_t *seed, uint64_t seed_off, size_t seed_len,
                       tx_line_t *out, uint8_t *raw)
{
    if (!b || !out || !raw) {
        return TEXT_ERR_PARAM;
    }

    out->off = pos;
    out->next = pos;
    out->len = 0;
    out->width = 0;
    out->chars = 0;
    if (pos >= b->size) {
        return 0;
    }

    const text_enc_t enc = (b->enc == TEXT_ENC_GBK) ? TEXT_ENC_GBK : TEXT_ENC_UTF8;
    tx_rd_t rd;
    rd_init(&rd, b, pos, seed, seed_off, seed_len);

    int width = 0;      /* 像素宽 */
    int cells = 0;      /* 占位格数 */
    int rawlen = 0;     /* raw 中字节数 */
    int n = 0;          /* 已放入本行的单位数 */
    uint64_t p = pos;   /* 当前源偏移(= rd.base + rd.idx) */
    uint64_t next = pos;
    bool stopped = false;
    bool saw_nl = false;
    bool last_open = false;

    /* 回退最后一个字所需的历史(每行最多 32 个单位) */
    uint64_t u_off[READER_CHARS_PER_LINE + 1];
    uint16_t u_w[READER_CHARS_PER_LINE + 1];
    uint8_t  u_cells[READER_CHARS_PER_LINE + 1];
    uint8_t  u_raw[READER_CHARS_PER_LINE + 1];

    for (;;) {
        if (p >= b->size) {
            break;                          /* EOF:本行结束 */
        }
        (void)rd_ensure(&rd, 4);            /* EOF 时会少于 4 B,按可用字节处理 */
        if (rd.err) {
            TX_LOGE(TAG, "read_at failed at %llu", (unsigned long long)p);
            out->next = p;
            return TEXT_ERR_IO;
        }
        const uint8_t *q = rd.buf + rd.idx;
        size_t avail = (size_t)(rd.len - rd.idx);
        if (avail == 0) {
            break;                          /* 防御:p<size 时不应发生 */
        }

        uint32_t cp = 0xFFFD;
        int nb = 1;
        bool pair = false;

        if (enc == TEXT_ENC_GBK) {
            if (q[0] < 0x80u) {
                cp = q[0];
                nb = 1;
            } else if (avail >= 2 && tx_gbk_lead_ok(q[0]) && tx_gbk_trail_ok(q[1])) {
                cp = TX_GBK_CP(q[0], q[1]);
                nb = 2;
                pair = true;
            } else {
                cp = 0xFFFD;                /* 解码失败:替换符 + 前进 1 B */
                nb = 1;
            }
        } else {
            int len = tx_utf8_seq_len(q[0]);
            if (len > 0 && (size_t)len <= avail && tx_utf8_check(q, len, &cp)) {
                nb = len;
            } else {
                cp = 0xFFFD;                /* 非法/截断:替换符 + 前进 1 B */
                nb = 1;
            }
        }

        if (cp == (uint32_t)'\n') {
            p += (uint64_t)nb;
            rd.idx += (uint32_t)nb;
            saw_nl = true;
            break;
        }
        if (cp == (uint32_t)'\r') {         /* 跳过,不占宽 */
            p += (uint64_t)nb;
            rd.idx += (uint32_t)nb;
            continue;
        }

        int w = tx_char_width(cp);
        if (w == 0) {                       /* 其余 <0x20 跳过 */
            p += (uint64_t)nb;
            rd.idx += (uint32_t)nb;
            continue;
        }

        int add_cells = (cp == (uint32_t)'\t') ? READER_TAB_SPACES : 1;
        int raw_need = (cp == (uint32_t)'\t') ? READER_TAB_SPACES : nb;

        bool fits = (n < READER_CHARS_PER_LINE) &&
                    (width + w <= READER_TEXT_MAX_W) &&
                    (cells + add_cells <= READER_CHARS_PER_LINE) &&
                    (rawlen + raw_need <= READER_LINE_MAX_BYTES);
        if (!fits && n > 0) {
            bool retract = false;
            if (n >= 2) {
                if (pair) {
                    retract = tx_is_closing_gbk(TX_GBK_HI(cp), TX_GBK_LO(cp));
                } else {
                    retract = tx_is_closing(cp);
                }
                /* 本行最后一字是起始标点 → 也一起退(只退一次) */
                if (!retract && last_open) {
                    retract = true;
                }
            }
            if (retract) {
                n--;
                width -= u_w[n];
                cells -= u_cells[n];
                rawlen = u_raw[n];
                next = u_off[n];
            } else {
                next = p;
            }
            stopped = true;
            break;
        }
        /* fits 为假且 n==0:强制吃 1 字,防止空行不前进(DESIGN §5.4) */

        u_off[n] = p;
        u_w[n] = (uint16_t)w;
        u_cells[n] = (uint8_t)add_cells;
        u_raw[n] = (uint8_t)rawlen;
        if (cp == (uint32_t)'\t') {
            memset(raw + rawlen, ' ', (size_t)READER_TAB_SPACES);
        } else {
            memcpy(raw + rawlen, q, (size_t)nb);   /* 源编码字节 */
        }
        rawlen += raw_need;
        width += w;
        cells += add_cells;
        n++;
        last_open = pair ? tx_is_opening_gbk(TX_GBK_HI(cp), TX_GBK_LO(cp))
                         : tx_is_opening(cp);
        p += (uint64_t)nb;
        rd.idx += (uint32_t)nb;
    }

    if (!stopped) {
        next = p;
    }
    out->next = next;
    out->len = (uint16_t)rawlen;
    out->width = (uint16_t)width;
    out->chars = (uint8_t)cells;

    if (n == 0 && !saw_nl) {
        return 0;                           /* 只剩被跳过的控制字符或已到 EOF */
    }
    /* 兜底:正常情况下 next 必然 > pos(每次循环至少前进 1 B) */
    if (out->next <= pos) {
        out->next = (pos + 1u <= b->size) ? pos + 1u : b->size;
    }
    return 1;
}

int tx_layout_line(const text_book_t *b, uint64_t pos, tx_line_t *out, uint8_t *raw)
{
    return layout_impl(b, pos, NULL, 0, 0, out, raw);
}

int tx_layout_line_seeded(const text_book_t *b, uint64_t pos,
                          const uint8_t *seed, uint64_t seed_off, size_t seed_len,
                          tx_line_t *out, uint8_t *raw)
{
    return layout_impl(b, pos, seed, seed_off, seed_len, out, raw);
}

/* ------------------------------------------------------------------ *
 * 字符边界与行对齐
 * ------------------------------------------------------------------ */
uint64_t tx_align_char_boundary(const text_book_t *b, uint64_t pos)
{
    if (!b || pos == 0) {
        return 0;
    }
    if (pos >= b->size) {
        return b->size;
    }

    uint64_t base = (pos > 4u) ? pos - 4u : 0;
    size_t n = (size_t)(pos - base) + 1u;   /* 含 pos 处的字节 */
    if (base + (uint64_t)n > b->size) {
        n = (size_t)(b->size - base);
    }
    uint8_t buf[6] = {0};
    if (b->src.read_at(b->src.ctx, base, buf, n) != 0) {
        return pos;                         /* 读失败:不动,布局会走替换符 */
    }
    size_t k = (size_t)(pos - base);
    if (k >= n) {
        return pos;
    }

    if (b->enc == TEXT_ENC_GBK) {
        if (k >= 1 && tx_gbk_lead_ok(buf[k - 1]) && tx_gbk_trail_ok(buf[k])) {
            return pos - 1u;
        }
        return pos;
    }

    /* UTF-8:最多回退 3 个续字节 */
    int steps = 0;
    while (steps < 3 && k > 0 && (buf[k] & 0xC0u) == 0x80u) {
        k--;
        steps++;
    }
    return base + (uint64_t)k;
}

/* 行首 = 文件开头(0),或前一个字节是 '\n'。
 *
 * 语义(text.h 契约,T1/T9):
 *   off == 0        -> 0                (0 本身就是第一行行首)
 *   off 已是行首    -> off              (不能跳过本行)
 *   off >= size     -> size
 *   其它            -> off 之后(含 off)第一个 '\n' 的下一个字节;没有则 size
 *
 * 从 off-1 开始一次读入,先看前一字节判断 off 是否已是行首,再往后找 '\n',
 * 避免为了判行首额外做一次 1 B 的 read_at。读失败返回 size,与旧实现一致,
 * 由调用方(书签/百分比跳转)按文件尾兜底。
 */
uint64_t text_align_line(text_book_t *b, uint64_t off)
{
    if (!b) {
        return 0;
    }
    if (off == 0) {
        return 0;
    }
    if (off >= b->size) {
        return b->size;
    }

    uint8_t buf[256];
    const uint64_t prev_off = off - 1u;
    uint64_t p = prev_off;
    while (p < b->size) {
        size_t want = sizeof(buf);
        if (b->size - p < (uint64_t)want) {
            want = (size_t)(b->size - p);
        }
        if (b->src.read_at(b->src.ctx, p, buf, want) != 0) {
            TX_LOGW(TAG, "align_line read_at failed at %llu",
                    (unsigned long long)p);
            return b->size;
        }
        /* 只有第一块含 off-1;off < size 保证 want >= 2,off 之后的字节也在本块 */
        size_t from = (p == prev_off) ? 1u : 0u;
        if (from == 1u && buf[0] == (uint8_t)'\n') {
            return off;                     /* off 本身已是行首 */
        }
        const uint8_t *nl = (want > from)
                                ? memchr(buf + from, '\n', want - from)
                                : NULL;
        if (nl) {
            return p + (uint64_t)(nl - buf) + 1u;
        }
        p += (uint64_t)want;
    }
    return b->size;
}
