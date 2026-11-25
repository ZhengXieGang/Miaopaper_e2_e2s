/* text_page.c — 无索引分页与页面渲染(DESIGN §5.5/§5.6)。
 *
 * 前向:从 pos 连排 READER_LINES_PER_PAGE(5)行,end = 消费位置;文件尾不足
 *       5 行时提前结束。
 * 后向:窗口 [max(0, S-1024), S+128) 一次读入,候选 = 窗口起点与窗口内每个
 *       '\n' 之后(只保留 ≤ S 的最近 64 个),从小到大正排 5 行,取第一个
 *       end == S 的候选;否则取包含 S-1 的页;再失败 start = max(0, S-240)
 *       退到字符边界并打 ESP_LOGW。前向不丢字;后向允许重复,窗口内没有
 *       任何行首候选时(如整段无换行)会回退到 S-240 附近。
 * 渲染:清成白底后逐行调用包内 tx_draw_line(raw 按 b->enc 解码,UTF-8/GBK
 *       都支持);公共 API text_draw_line 只接 UTF-8,不用于这里。
 */
#include <stddef.h>
#include <stdint.h>

#include "text.h"

#include <string.h>

#include "gfx.h"

#include "text_internal.h"

static const char *TAG = "text";

/* 后向分页最后兜底的回退距离(DESIGN §5.5:start = max(0, S-240)) */
#define TX_BACK_FALLBACK 240u

/* 按需使用预读窗口排一页(seed 可为 NULL)。 */
static int layout_into(text_book_t *b, uint64_t pos,
                       const uint8_t *seed, uint64_t seed_off, size_t seed_len,
                       text_page_t *pg)
{
    memset(pg, 0, sizeof(*pg));
    pg->start = pos;

    uint64_t cur = pos;
    int n = 0;
    while (n < READER_LINES_PER_PAGE) {
        tx_line_t ln;
        int rc = seed ? tx_layout_line_seeded(b, cur, seed, seed_off, seed_len,
                                              &ln, pg->raw[n])
                      : tx_layout_line(b, cur, &ln, pg->raw[n]);
        if (rc < 0) {
            pg->line_count = (uint8_t)n;
            pg->end = cur;
            return rc;
        }
        if (rc == 0) {
            break;                  /* EOF:本页提前结束 */
        }
        pg->line[n].off = ln.off;
        pg->line[n].len = ln.len;
        pg->line[n].width = ln.width;
        cur = ln.next;
        n++;
    }
    pg->line_count = (uint8_t)n;
    pg->end = cur;
    return TEXT_OK;
}

int text_page_forward(text_book_t *b, uint64_t pos, text_page_t *pg)
{
    if (!b || !pg) {
        return TEXT_ERR_PARAM;
    }
    uint64_t start = (pos > b->size) ? b->size : pos;
    return layout_into(b, start, NULL, 0, 0, pg);
}

int text_page_backward(text_book_t *b, uint64_t pos, text_page_t *pg)
{
    if (!b || !pg) {
        return TEXT_ERR_PARAM;
    }
    if (b->size == 0) {
        return text_page_forward(b, 0, pg);
    }
    uint64_t s = (pos > b->size) ? b->size : pos;
    if (s == 0) {
        return text_page_forward(b, 0, pg);
    }

    /* 一次读入 [max(0,S-1024), S+128) */
    uint64_t win_start = (s > (uint64_t)READER_PAGE_WINDOW)
                             ? s - (uint64_t)READER_PAGE_WINDOW
                             : 0;
    uint64_t win_end = s + (uint64_t)READER_PAGE_AHEAD;
    if (win_end > b->size) {
        win_end = b->size;
    }
    size_t win_len = (size_t)(win_end - win_start);
    uint8_t win[READER_PAGE_WINDOW + READER_PAGE_AHEAD];
    if (b->src.read_at(b->src.ctx, win_start, win, win_len) != 0) {
        TX_LOGE(TAG, "backward window read failed at %llu",
                (unsigned long long)win_start);
        return TEXT_ERR_IO;
    }

    /* 候选:窗口起点 + 每个 '\n' 之后;只收 ≤ S 的,超过 64 个丢最旧的。
     * 真正的"页首"有两种来源:
     *   1) 行首(前一字节是 '\n'):一个页只有 5 行,每行最多消费一个 '\n',
     *      所以它一定在最后 5 个候选之内,64 的上限不会把它丢掉;
     *   2) 行内按宽度断开的页首(前一字节是普通字符):它本来就不在候选集里,
     *      只能落到下面的"包含 S-1"或 S-240 兜底。此时向后翻会与前一页重叠,
     *      甚至(无换行的整段 ASCII)落点比上一页更靠前——DESIGN §5.5 明确
     *      "允许重复",这里只保证向前读不丢字,不保证后向逐页精确。
     * 因此 64 个候选是 DESIGN §5.5 的"最多 64 个",不是缺陷;若以后要后向
     * 逐页精确,需要把行内断行位置也纳入候选(会增加布局次数)。 */
    uint64_t cand[READER_PAGE_CANDIDATES];
    int nc = 0;
    cand[nc++] = win_start;
    for (size_t i = 0; i + 1u < win_len; i++) {
        if (win[i] != (uint8_t)'\n') {
            continue;
        }
        uint64_t c = win_start + (uint64_t)i + 1u;
        if (c > s) {
            break;
        }
        if (cand[nc - 1] == c) {
            continue;
        }
        if (nc == (int)READER_PAGE_CANDIDATES) {
            memmove(&cand[0], &cand[1],
                    (size_t)(nc - 1) * sizeof(cand[0]));
            nc--;
        }
        cand[nc++] = c;
    }

    /* 从小到大正排 5 行,取第一个 end == S;同时记住包含 S-1 的候选 */
    uint64_t best = 0;
    bool best_valid = false;
    int rc = TEXT_OK;
    for (int i = 0; i < nc; i++) {
        uint64_t c = cand[i];
        if (c > s) {
            break;
        }
        rc = layout_into(b, c, win, win_start, win_len, pg);
        if (rc < 0) {
            return rc;
        }
        if (pg->end == s) {
            return TEXT_OK;
        }
        if (c < s && pg->end >= s && !best_valid) {
            /* 取包含 S-1 的页里最靠前的一个:往回退得最远又不断字 */
            best = c;
            best_valid = true;
        }
    }
    if (best_valid) {
        return layout_into(b, best, win, win_start, win_len, pg);
    }

    /* 最后兜底:start = max(0, S-240),退到字符边界;不丢字,允许重复 */
    uint64_t start = (s > (uint64_t)TX_BACK_FALLBACK) ? s - (uint64_t)TX_BACK_FALLBACK : 0;
    start = tx_align_char_boundary(b, start);
    TX_LOGW(TAG, "backward fallback pos=%llu start=%llu",
            (unsigned long long)s, (unsigned long long)start);
    return layout_into(b, start, win, win_start, win_len, pg);
}

void text_render_page(const text_book_t *b, const text_page_t *pg, uint8_t *fb)
{
    if (!fb) {
        return;
    }
    gfx_clear(fb, 0);           /* 1=白 */
    if (!b || !pg) {
        return;
    }
    const text_enc_t enc = (b->enc == TEXT_ENC_GBK) ? TEXT_ENC_GBK : TEXT_ENC_UTF8;
    int lines = pg->line_count;
    if (lines > READER_LINES_PER_PAGE) {
        lines = READER_LINES_PER_PAGE;
    }
    for (int i = 0; i < lines; i++) {
        size_t len = pg->line[i].len;
        if (len > READER_LINE_MAX_BYTES) {
            len = READER_LINE_MAX_BYTES;
        }
        int y = READER_CONTENT_TOP + i * READER_LINE_H;
        (void)tx_draw_line(fb, READER_TEXT_MARGIN_X, y, pg->raw[i], len,
                           READER_TEXT_MAX_W, 1, enc);
    }
}
