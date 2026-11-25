/* mp_ui.c — 250x122 1bpp 界面渲染（纯 C：不碰 ESP-IDF、不读文件）。
 *
 * 版式真源：docs/PORT_SPEC.md §1 与 docs/COPY_DECK.md §2/§4；
 * 本文件只出现 COPY_DECK 里有的句子。
 *
 * 2026-11-25 UI v3（统一、简洁、对称）：
 *   - 状态栏：左标题 / 居中时间(或向导步数) / 蓝牙开显示符文(关则隐藏) / 描边黑芯电池 + y=18 分隔线；
 *   - 主页只有 Inter 66 px 时钟（无分隔线）与居中底栏；
 *   - 阅读正文 5 行 + 2 px 进度条；顶栏不显示进度；
 *   - 书库与设置统一使用微圆角整行反色高亮条 + 极简滚动条，设置存储行内联容量条；
 *   - 书籍信息页采用元信息 + 点线 + 2x2 圆角按钮网格；
 *   - 时间设置展示完整 YYYY-MM-DD / HH:MM 并用反色块高亮当前编辑字段；
 *   - 跳转页采用 26 px 中号数值 + 带刻度的 200 px 圆形滑块轨道；
 *   - 锁屏阅读数据统计页采用上下点线 + 双栏数字 + 迷你锁头 + 长按[○]解锁；
 *   - 消息/确认/完成页使用统一体量 1 px 图标与垂直居中排版；
 *   - 底栏键帽无方框、对称居中排版，返回/取消采用 × 符号。
 *
 * 不变量：
 *   - 所有绘制都走 gfx_* / mp_ui_text / mp_text_draw（内部逐点裁剪），
 *     落到 250x122 之外的点被丢弃；
 *   - 屏上文案只用 COPY_DECK 的句子，键名一律画键帽图标（mp_hints.c）。
 */
#include "mp_ui.h"

#include <stdio.h>
#include <string.h>

#include "gfx.h"
#include "mp_font.h"
#include "mp_hints.h"
#include "text.h"

#define UI_BASELINE 14          /* Inter Regular 16 px mode=1：ink -12..0，落到行顶+2..+14 */
#define UI_TEXT_H 16

/* 锁屏兜底页与阅读统计锁屏页文案（COPY_DECK §2.6）。 */
static const char TXT_LOCKED[] = "已锁定";
static const char TXT_HOLD[] = "长按";
static const char TXT_UNLOCK[] = "解锁";
static const char TXT_READ_MINS[] = "阅读分钟";
static const char TXT_READ_CHARS[] = "阅读字数";
static const char TXT_WAN[] = "万";
static const char TXT_BL_QUICK_PREFIX[] = "长按";
static const char TXT_BL_QUICK_SUFFIX[] = "快速调整背光";
static const char TXT_BL_LABEL[] = "背光";
static const char TXT_BL_ADJUST[] = "调整";
static const char TXT_BL_CONFIRM[] = "确定";
static const char TXT_LOW_BATT_1[] = "电量耗尽";
static const char TXT_LOW_BATT_2[] = "请充电";
static const char *const k_bl_level_names[4] = {"关闭", "1档", "2档", "3档"};

/* 5x7 电量及刻度数字；bit4 是最左像素，'0' 不带斜杠。 */
static const uint8_t k_digit5x7[10][7] = {
    {0x0E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E},
    {0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E},
    {0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F},
    {0x1F, 0x02, 0x04, 0x02, 0x01, 0x11, 0x0E},
    {0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02},
    {0x1F, 0x10, 0x1E, 0x01, 0x01, 0x11, 0x0E},
    {0x06, 0x08, 0x10, 0x1E, 0x11, 0x11, 0x0E},
    {0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08},
    {0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E},
    {0x0E, 0x11, 0x11, 0x0F, 0x01, 0x02, 0x0C},
};

/* 5x7 百分号 '%'（用于跳转刻度两端标注）。 */
static const uint8_t k_percent5x7[7] = {
    0x19, 0x1A, 0x04, 0x04, 0x08, 0x0B, 0x13,
};

/* ------------------------------------------------------------------ *
 * 小工具
 * ------------------------------------------------------------------ */
static int ui_min(int a, int b)
{
    return a < b ? a : b;
}

static int ui_max(int a, int b)
{
    return a > b ? a : b;
}

/* 四角削去 1 px 的微圆角实心矩形。 */
static void ui_rrect_fill(uint8_t *fb, int x, int y, int w, int h, int black)
{
    if (!fb || w <= 0 || h <= 0) {
        return;
    }
    if (w < 4 || h < 4) {
        gfx_rect(fb, x, y, w, h, black);
        return;
    }
    gfx_hline(fb, x + 1, x + w - 2, y, black);
    if (h > 2) {
        gfx_rect(fb, x, y + 1, w, h - 2, black);
    }
    gfx_hline(fb, x + 1, x + w - 2, y + h - 1, black);
}

/* 四角削去 1 px 的微圆角 1 px 描边矩形。 */
static void ui_rrect_frame(uint8_t *fb, int x, int y, int w, int h, int black)
{
    if (!fb || w <= 0 || h <= 0) {
        return;
    }
    if (w < 4 || h < 4) {
        gfx_frame(fb, x, y, w, h, black);
        return;
    }
    gfx_hline(fb, x + 1, x + w - 2, y, black);
    gfx_hline(fb, x + 1, x + w - 2, y + h - 1, black);
    gfx_vline(fb, x, y + 1, y + h - 2, black);
    gfx_vline(fb, x + w - 1, y + 1, y + h - 2, black);
}

/* 步进 3 px 的轻盈点线（1 px 黑 + 2 px 白）。 */
static void ui_dotted_hline(uint8_t *fb, int x0, int x1, int y)
{
    for (int x = x0; x <= x1; x += 3) {
        gfx_px(fb, x, y, 1);
    }
}

static void ui_line(uint8_t *fb, int x0, int y0, int x1, int y1, int black)
{
    int dx = x1 > x0 ? x1 - x0 : x0 - x1;
    int dy = y1 > y0 ? y1 - y0 : y0 - y1;
    int sx = x0 < x1 ? 1 : -1;
    int sy = y0 < y1 ? 1 : -1;
    int err = dx - dy;

    for (;;) {
        gfx_px(fb, x0, y0, black);
        if (x0 == x1 && y0 == y1) {
            break;
        }
        int e2 = err * 2;
        if (e2 > -dy) {
            err -= dy;
            x0 += sx;
        }
        if (e2 < dx) {
            err += dx;
            y0 += sy;
        }
    }
}

static void ui_glyph5x7(uint8_t *fb, int x, int y, const uint8_t rows[7],
                        int black)
{
    for (int row = 0; row < 7; row++) {
        uint8_t bits = rows[row];
        for (int col = 0; col < 5; col++) {
            if (bits & (uint8_t)(0x10u >> col)) {
                gfx_px(fb, x + col, y + row, black);
            }
        }
    }
}

static void ui_digit5x7(uint8_t *fb, int x, int y, unsigned d, int black)
{
    if (d > 9) {
        return;
    }
    ui_glyph5x7(fb, x, y, k_digit5x7[d], black);
}

/* "85" / "100"：数字之间 1 px，返回实绘宽度；可选追加 '%'。 */
static int ui_number5x7_opt(uint8_t *fb, int x, int y, unsigned v, int black,
                            int with_pct)
{
    char buf[4];
    int n = snprintf(buf, sizeof buf, "%u", v > 100 ? 100u : v);
    if (n <= 0) {
        return 0;
    }
    for (int i = 0; i < n; i++) {
        ui_digit5x7(fb, x + i * 6, y, (unsigned)(buf[i] - '0'), black);
    }
    if (with_pct) {
        ui_glyph5x7(fb, x + n * 6, y, k_percent5x7, black);
        return n * 6 + 5;
    }
    return n * 6 - 1;
}

/* 按水平分割线 split_x 反色绘制 5x7 数字：px < split_x 画白(0)，否则画黑(1)。 */
static void ui_number5x7_split(uint8_t *fb, int x, int y, unsigned v,
                               int split_x)
{
    char buf[4];
    int n = snprintf(buf, sizeof buf, "%u", v > 100 ? 100u : v);
    if (n <= 0) {
        return;
    }
    for (int i = 0; i < n; i++) {
        const uint8_t *rows = k_digit5x7[(unsigned)(buf[i] - '0')];
        int gx = x + i * 6;
        for (int row = 0; row < 7; row++) {
            uint8_t bits = rows[row];
            for (int col = 0; col < 5; col++) {
                if (bits & (uint8_t)(0x10u >> col)) {
                    int px = gx + col;
                    gfx_px(fb, px, y + row, px < split_x ? 0 : 1);
                }
            }
        }
    }
}

/* 计算 "v%" 在 5x7 字体下的像素宽度（数字宽 5、字距 1、'%' 宽 5）。 */
static int ui_pct5x7_w(unsigned v)
{
    int digits = (v >= 100u) ? 3 : (v >= 10u ? 2 : 1);
    return digits * 6 + 5;
}

/* 16 px 行里的 UTF-8 ASCII 段：Inter_UI_16，基线 top+UI_BASELINE。 */
static int ascii_advance_run(const char *s, size_t n)
{
    int w = 0;
    for (size_t i = 0; i < n; i++) {
        char one[2] = {s[i], '\0'};
        w += mp_gfx_width(&Inter_UI_16, one);
    }
    return w;
}

static int ui_utf8_len(unsigned char c)
{
    if (c < 0x80) {
        return 1;
    }
    if ((c & 0xE0) == 0xC0) {
        return 2;
    }
    if ((c & 0xF0) == 0xE0) {
        return 3;
    }
    if ((c & 0xF8) == 0xF0) {
        return 4;
    }
    return 1;
}

static int ui_text_width(const char *s)
{
    if (!s) {
        return 0;
    }
    int w = 0;
    const unsigned char *p = (const unsigned char *)s;
    while (*p) {
        if (*p >= 0x20 && *p <= 0x7E) {
            const unsigned char *q = p;
            while (*q >= 0x20 && *q <= 0x7E) {
                q++;
            }
            w += ascii_advance_run((const char *)p, (size_t)(q - p));
            p = q;
        } else {
            const unsigned char *q = p;
            while (*q && (*q < 0x20 || *q > 0x7E)) {
                int n = ui_utf8_len(*q);
                q += n;
            }
            char buf[128];
            size_t n = (size_t)(q - p);
            if (n >= sizeof buf) {
                n = sizeof buf - 1;
            }
            memcpy(buf, p, n);
            buf[n] = '\0';
            w += text_measure(buf);
            p = q;
        }
    }
    return w;
}

static void ui_text_color(uint8_t *fb, int x, int top, const char *s, int max_w,
                          int black)
{
    if (!fb || !s) {
        return;
    }
    int limit = max_w > 0 ? max_w : MP_FB_W - x;
    if (limit <= 0) {
        return;
    }

    int cx = x;
    const unsigned char *p = (const unsigned char *)s;
    while (*p) {
        if (*p >= 0x20 && *p <= 0x7E) {
            char run[64];
            size_t n = 0;
            while (p[n] >= 0x20 && p[n] <= 0x7E && n + 1 < sizeof run) {
                run[n] = (char)p[n];
                n++;
            }
            run[n] = '\0';
            int w = ascii_advance_run(run, n);
            if (w > limit - (cx - x)) {
                /* 按字形 advance 截断。 */
                size_t cut = 0;
                int used = 0;
                while (cut < n) {
                    char one[2] = {run[cut], '\0'};
                    int adv = mp_gfx_width(&Inter_UI_16, one);
                    if (used + adv > limit - (cx - x)) {
                        break;
                    }
                    used += adv;
                    cut++;
                }
                run[cut] = '\0';
                w = used;
            }
            if (w > 0) {
                mp_gfx_text_color(fb, &Inter_UI_16, cx, top + UI_BASELINE,
                                  run, black);
                cx += w;
            }
            p += n;
            if (w == 0 || cx - x >= limit) {
                break;
            }
        } else {
            char run[128];
            size_t n = 0;
            while (*p && (*p < 0x20 || *p > 0x7E)) {
                int len = ui_utf8_len(*p);
                if (n + (size_t)len >= sizeof run) {
                    break;
                }
                for (int i = 0; i < len && p[i]; i++) {
                    run[n++] = (char)p[i];
                }
                p += len;
            }
            run[n] = '\0';
            int rem = limit - (cx - x);
            if (rem <= 0) {
                break;
            }
            int w = text_measure(run);
            text_draw_line(fb, cx, top, run, rem, black);
            if (w > rem) {
                break;
            }
            cx += w;
        }
    }
}

/* mp_str_enc_t -> 绘制路径。 */
static void draw_str_color(uint8_t *fb, int x, int top, const mp_str_t *s,
                           int max_w, int black)
{
    if (!fb || !s || !s->text) {
        return;
    }
    if (s->enc == MP_STR_GB2312) {
        mp_ui_gb_text_color(fb, x, top, s->text, MP_ENC_GB2312, max_w, black);
    } else if (s->enc == MP_STR_ASCII) {
        mp_ui_gb_text_color(fb, x, top, s->text, MP_ENC_ASCII, max_w, black);
    } else {
        ui_text_color(fb, x, top, s->text, max_w, black);
    }
}

static void draw_str(uint8_t *fb, int x, int top, const mp_str_t *s, int max_w)
{
    draw_str_color(fb, x, top, s, max_w, 1);
}

/* ------------------------------------------------------------------ *
 * 状态栏
 * ------------------------------------------------------------------ */
static void bt_icon(uint8_t *fb, int x, int y)
{
    /* 12x13 标准蓝牙符文（左斜入 + 中轴竖线 + 右侧上下闭合三角）。 */
    static const uint16_t rows[13] = {
        0x040, 0x060, 0x050, 0x248, 0x150, 0x0E0, 0x040,
        0x0E0, 0x150, 0x248, 0x050, 0x060, 0x040,
    };
    for (int j = 0; j < 13; j++) {
        for (int i = 0; i < 12; i++) {
            if (rows[j] & (uint16_t)(0x800u >> i)) {
                gfx_px(fb, x + i, y + j, 1);
            }
        }
    }
}

void mp_ui_battery(uint8_t *fb, int x, int y, uint8_t pct, uint8_t valid,
                   uint8_t charging)
{
    if (!fb) {
        return;
    }
    if (pct > 100) {
        pct = 100;
    }

    ui_rrect_frame(fb, x, y, MP_BATT_W, MP_BATT_H, 1);
    gfx_rect(fb, x + MP_BATT_W, y + 4, MP_BATT_CAP_W, MP_BATT_CAP_H, 1);

    if (charging) {
        gfx_hline(fb, x - 7, x - 3, y + 6, 1);
        gfx_vline(fb, x - 5, y + 4, y + 8, 1);
    }
    if (!valid) {
        return;
    }

    /* 外框内留 1 px 白隙后，按真实电量比例从左向右填充 22x9 黑芯，
     * 居中 5x7 电量数字按所在底层像素颜色逐点反色。 */
    int inner_w = MP_BATT_W - 4;
    int fill_w = (inner_w * (int)pct + 50) / 100;
    if (pct > 0 && fill_w == 0) {
        fill_w = 1;
    }
    if (fill_w > 0) {
        gfx_rect(fb, x + 2, y + 2, fill_w, MP_BATT_H - 4, 1);
    }
    char buf[4];
    int n = snprintf(buf, sizeof buf, "%u", (unsigned)pct);
    int w = n * 5 + (n - 1);
    ui_number5x7_split(fb, x + (MP_BATT_W - w) / 2, y + 3, (unsigned)pct,
                       x + 2 + fill_w);
}

void mp_ui_status_bar(uint8_t *fb, const mp_ui_state_t *st)
{
    if (!fb || !st) {
        return;
    }

    draw_str(fb, MP_STATUS_X, MP_STATUS_TEXT_TOP, &st->left,
             MP_STATUS_TITLE_MAX_W);

    if (st->time_valid) {
        char t[8];
        int hh = st->hour < 24 ? (int)st->hour : 0;
        int mm = st->min < 60 ? (int)st->min : 0;
        snprintf(t, sizeof t, "%02d:%02d", hh, mm);
        int w = ui_text_width(t);
        int x = (MP_FB_W - w) / 2;
        ui_text_color(fb, ui_max(x, 0), MP_STATUS_TIME_TOP, t, w, 1);
    } else if (st->mid && st->mid[0]) {
        /* 无时间时（如时间设置向导），将步骤指示水平居中于状态栏。 */
        int w = ui_text_width(st->mid);
        int x = (MP_FB_W - w) / 2;
        ui_text_color(fb, ui_max(x, 0), MP_STATUS_TEXT_TOP, st->mid, w, 1);
    }

    if (st->bt_on) {
        bt_icon(fb, MP_BT_X, MP_BT_Y);
    }
    mp_ui_battery(fb, MP_BATT_X, MP_BATT_Y, st->battery, st->battery_valid,
                  st->charging);

    /* UI v3：y=18 细分隔线；CLOCK 主页保持纯时钟，不画线。 */
    if (st->screen != MP_SCREEN_CLOCK) {
        gfx_hline(fb, 0, MP_FB_W - 1, MP_RULE_Y, 1);
    }
}

/* ------------------------------------------------------------------ *
 * CLOCK
 * ------------------------------------------------------------------ */
void mp_ui_clock(uint8_t *fb, const mp_ui_state_t *st)
{
    if (!fb || !st) {
        return;
    }

    char t[8];
    int hh = st->hour < 24 ? (int)st->hour : 0;
    int mm = st->min < 60 ? (int)st->min : 0;
    snprintf(t, sizeof t, "%02d:%02d", hh, mm);

    int w = mp_gfx_width(&Inter_Clock_66, t);
    int x = (MP_FB_W - w) / 2;
    mp_gfx_text(fb, &Inter_Clock_66, ui_max(x, 0), MP_CLOCK_BASE, t);
}

/* ------------------------------------------------------------------ *
 * 书库 & 设置共用列表工具（微圆角反色条 + 右侧极简滚动条）
 * ------------------------------------------------------------------ */
static uint8_t list_visible(const mp_ui_state_t *st)
{
    int n;
    if (st->row_count <= st->scroll) {
        return 0;
    }
    n = (int)st->row_count - (int)st->scroll;
    return (uint8_t)ui_min(n, MP_LIST_ROWS);
}

/* 超过 4 项时在右侧 x=243..244, y=21..101 绘制轻盈滚动指示条。 */
static void ui_scrollbar(uint8_t *fb, uint8_t scroll, uint8_t total)
{
    if (total <= MP_LIST_ROWS) {
        return;
    }
    const int tx = 243;
    const int ty = 21;
    const int th = 81;
    for (int y = ty; y < ty + th; y += 3) {
        gfx_px(fb, tx, y, 1);
    }
    int kh = (th * MP_LIST_ROWS) / (int)total;
    if (kh < 12) {
        kh = 12;
    }
    int max_s = (int)total - MP_LIST_ROWS;
    int s = (int)scroll > max_s ? max_s : (int)scroll;
    int ky = ty + ((th - kh) * s + max_s / 2) / max_s;
    gfx_rect(fb, tx, ky, 2, kh, 1);
}

void mp_ui_list(uint8_t *fb, const mp_ui_state_t *st)
{
    if (!fb || !st) {
        return;
    }

    int has_scroll = (st->row_count > MP_LIST_ROWS);
    int bar_w = has_scroll ? 233 : MP_SEL_BAR_W;
    uint8_t n = list_visible(st);

    for (uint8_t i = 0; i < n; i++) {
        int logical = (int)st->scroll + (int)i;
        int top = MP_LIST_TOP + (int)i * MP_LIST_STEP;
        const mp_ui_row_t *r = &st->row[i];
        int selected = ((int)st->sel == logical);
        int fg = selected ? 0 : 1;

        if (selected) {
            ui_rrect_fill(fb, MP_SEL_BAR_X, top, bar_w, MP_SEL_BAR_H, 1);
        }
        if (r->label) {
            mp_str_t s = {r->label, r->enc};
            draw_str_color(fb, 12, top + 2, &s, bar_w - 14, fg);
        }
    }
    if (has_scroll) {
        ui_scrollbar(fb, st->scroll, st->row_count);
    }
}

/* ------------------------------------------------------------------ *
 * SETTINGS（微圆角整行反色 + 右侧开关/数值/内联存储条）
 * ------------------------------------------------------------------ */
static const uint8_t k_pill12[12] = {4, 2, 1, 1, 0, 0, 0, 0, 1, 1, 2, 4};

static void pill_fill12(uint8_t *fb, int x, int y, int w, int black)
{
    for (int row = 0; row < 12; row++) {
        int in = k_pill12[row];
        if (w - 2 * in > 0) {
            gfx_hline(fb, x + in, x + w - 1 - in, y + row, black);
        }
    }
}

/* 严格 1 px 闭合圆角胶囊描边（26x12 关闭态外框）。 */
static void pill_outline12(uint8_t *fb, int x, int y, int w, int black)
{
    for (int row = 0; row < 12; row++) {
        int in = k_pill12[row];
        int lx = x + in;
        int rx = x + w - 1 - in;
        if (lx > rx) {
            continue;
        }
        if (row == 0 || row == 11) {
            gfx_hline(fb, lx, rx, y + row, black);
            continue;
        }
        int prev_in = k_pill12[row - 1];
        int next_in = k_pill12[row + 1];
        int side_in = prev_in > next_in ? prev_in : next_in;
        int span = side_in - in;
        if (span < 1) {
            span = 1;
        }
        for (int k = 0; k < span && lx + k <= rx - k; k++) {
            gfx_px(fb, lx + k, y + row, black);
            gfx_px(fb, rx - k, y + row, black);
        }
    }
}

static void knob(uint8_t *fb, int cx, int cy, int black)
{
    static const uint8_t rows[6] = {0x1E, 0x3F, 0x3F, 0x3F, 0x3F, 0x1E};
    for (int j = 0; j < 6; j++) {
        for (int i = 0; i < 6; i++) {
            if (rows[j] & (uint8_t)(0x20u >> i)) {
                gfx_px(fb, cx - 3 + i, cy - 3 + j, black);
            }
        }
    }
}

/* 26x12 胶囊开关；fg=1 白底黑行，fg=0 反色行。 */
static void switch_icon(uint8_t *fb, int x, int y, uint8_t on, int fg)
{
    if (on) {
        pill_fill12(fb, x, y, 26, fg);
        knob(fb, x + 20, y + 6, !fg);
    } else {
        pill_fill12(fb, x, y, 26, !fg);
        pill_outline12(fb, x, y, 26, fg);
        knob(fb, x + 6, y + 6, fg);
    }
}

static void chevron(uint8_t *fb, int x, int y, int black)
{
    ui_line(fb, x, y, x + 3, y + 4, black);
    ui_line(fb, x + 3, y + 4, x, y + 8, black);
}

void mp_ui_settings(uint8_t *fb, const mp_ui_state_t *st)
{
    if (!fb || !st) {
        return;
    }

    int has_scroll = (st->row_count > MP_LIST_ROWS);
    int bar_w = has_scroll ? 233 : MP_SEL_BAR_W;
    int val_right = has_scroll ? 233 : 236;
    uint8_t n = list_visible(st);

    for (uint8_t i = 0; i < n; i++) {
        int logical = (int)st->scroll + (int)i;
        int top = MP_LIST_TOP + (int)i * MP_LIST_STEP;
        const mp_ui_row_t *r = &st->row[i];
        int selected = ((int)st->sel == logical);
        int fg = selected ? 0 : 1;

        if (selected) {
            ui_rrect_fill(fb, MP_SEL_BAR_X, top, bar_w, MP_SEL_BAR_H, 1);
        }
        if (r->label) {
            ui_text_color(fb, 12, top + 2, r->label, 148, fg);
        }

        if (r->kind == MP_ROW_TOGGLE) {
            switch_icon(fb, val_right - 26, top + 4, r->on, fg);
        } else if (r->kind == MP_ROW_VALUE && r->value) {
            int w = ui_text_width(r->value);
            int x = val_right - w;
            ui_text_color(fb, ui_max(x, 12), top + 2, r->value, w, fg);
        } else if (r->kind == MP_ROW_STORAGE) {
            int vw = r->value ? ui_text_width(r->value) : 0;
            int vx = val_right - vw;
            if (r->value) {
                ui_text_color(fb, ui_max(vx, 12), top + 2, r->value, vw, 1);
            }
            /* 单行内联容量条：严格垂直居中，不碰字符也不撞下一行。 */
            if (r->value && strchr(r->value, '/')) {
                int bar_x = 54;
                int bar_w_in = vx - 10 - bar_x;
                if (bar_w_in >= 32) {
                    ui_rrect_frame(fb, bar_x, top + 7, bar_w_in, 6, 1);
                    int bar = r->bar > 100 ? 100 : r->bar;
                    int bw = ((bar_w_in - 2) * bar + 50) / 100;
                    if (bar > 0 && bw == 0) {
                        bw = 1;
                    }
                    if (bw > 0) {
                        gfx_rect(fb, bar_x + 1, top + 8, bw, 4, 1);
                    }
                }
            }
        } else if (r->kind == MP_ROW_ACTION) {
            chevron(fb, val_right - 5, top + 6, fg);
        }
    }
    if (has_scroll) {
        ui_scrollbar(fb, st->scroll, st->row_count);
    }
}

/* ------------------------------------------------------------------ *
 * ABOUT（产品居中标题 + 点线 + 键值规格表）
 * ------------------------------------------------------------------ */
void mp_ui_about(uint8_t *fb, const mp_ui_state_t *st)
{
    if (!fb || !st) {
        return;
    }

    uint8_t n = st->about_lines;
    if (n > MP_LIST_ROWS) {
        n = MP_LIST_ROWS;
    }
    if (n == 0) {
        return;
    }

    if (st->about_line[0].text) {
        if (st->about_line[0].enc == MP_STR_UTF8) {
            mp_ui_text_center(fb, 22, st->about_line[0].text);
        } else {
            draw_str(fb, 12, 22, &st->about_line[0], 226);
        }
    }
    ui_dotted_hline(fb, 12, 237, 40);

    for (uint8_t i = 1; i < n; i++) {
        const mp_str_t *line = &st->about_line[i];
        int top = 45 + (int)(i - 1) * 19;
        if (!line->text) {
            continue;
        }
        const char *sp = (line->enc == MP_STR_UTF8) ? strchr(line->text, ' ')
                                                    : NULL;
        if (sp && sp > line->text && sp[1] != '\0') {
            char key[64];
            size_t kl = (size_t)(sp - line->text);
            if (kl >= sizeof key) {
                kl = sizeof key - 1;
            }
            memcpy(key, line->text, kl);
            key[kl] = '\0';
            const char *val = sp + 1;
            int vw = ui_text_width(val);
            int vx = ui_max(238 - vw, 12);
            ui_text_color(fb, 12, top, key, vx - 12, 1);
            ui_text_color(fb, vx, top, val, vw, 1);
        } else {
            draw_str(fb, 12, top, line, 226);
        }
    }
}

/* ------------------------------------------------------------------ *
 * READ
 * ------------------------------------------------------------------ */
void mp_ui_progress(uint8_t *fb, uint8_t pct)
{
    if (!fb) {
        return;
    }
    if (pct > 100) {
        pct = 100;
    }

    /* 2 px 高：空态 1 px 底线，已读 2 px 实心。 */
    gfx_hline(fb, MP_PROGRESS_X, MP_PROGRESS_X + MP_PROGRESS_W - 1,
              MP_PROGRESS_Y + 1, 1);
    int w = (MP_PROGRESS_W * (int)pct + 50) / 100;
    if (pct > 0 && w == 0) {
        w = 1;
    }
    if (w > 0) {
        gfx_rect(fb, MP_PROGRESS_X, MP_PROGRESS_Y, w, MP_PROGRESS_H, 1);
    }
}

void mp_ui_read(uint8_t *fb, const mp_ui_state_t *st)
{
    if (!fb || !st) {
        return;
    }

    uint8_t n = st->line_count;
    if (n > MP_READ_LINES) {
        n = MP_READ_LINES;
    }
    int base_off = 12;               /* HZK 顶=行顶；ASCII Dialog 顶=行顶+0 */
    for (uint8_t i = 0; i < n; i++) {
        int top = MP_READ_TOP + (int)i * MP_READ_STEP;
        mp_text_draw(fb, st->line[i], st->line_enc, MP_READ_X, top + base_off,
                     MP_READ_W);
    }

    mp_ui_progress(fb, st->percent);
}

/* ------------------------------------------------------------------ *
 * 整页消息（EMPTY / ERROR / RESTORE / DELETE / TIME_DONE）
 * ------------------------------------------------------------------ */
static void msg_icon_book(uint8_t *fb, int x, int y)
{
    /* 24x20 翻开书本：带顶部书页折角、中脊书脊线与页内横线。 */
    gfx_hline(fb, x + 2, x + 8, y + 2, 1);
    gfx_hline(fb, x + 15, x + 21, y + 2, 1);
    gfx_px(fb, x + 9, y + 3, 1);
    gfx_px(fb, x + 14, y + 3, 1);
    gfx_px(fb, x + 10, y + 4, 1);
    gfx_px(fb, x + 13, y + 4, 1);
    gfx_vline(fb, x + 1, y + 3, y + 17, 1);
    gfx_vline(fb, x + 22, y + 3, y + 17, 1);
    gfx_rect(fb, x + 11, y + 5, 2, 14, 1);
    for (int row = 6; row <= 12; row += 3) {
        gfx_hline(fb, x + 4, x + 8, y + row, 1);
        gfx_hline(fb, x + 15, x + 19, y + row, 1);
    }
    gfx_hline(fb, x + 2, x + 8, y + 15, 1);
    gfx_hline(fb, x + 15, x + 21, y + 15, 1);
    gfx_px(fb, x + 9, y + 16, 1);
    gfx_px(fb, x + 14, y + 16, 1);
    gfx_hline(fb, x + 1, x + 22, y + 17, 1);
}

static void msg_icon_warn(uint8_t *fb, int x, int y)
{
    /* 24x20 严格对称加粗圆角警告三角（2 px 匀称线宽，与 msg_icon_circle 一致）+ 居中 2 px 感叹号。 */
    static const uint32_t rows[20] = {
        0x001800u, 0x003C00u, 0x003C00u, 0x006600u, 0x00C300u,
        0x00C300u, 0x018180u, 0x0300C0u, 0x0300C0u, 0x060060u,
        0x060060u, 0x0C0030u, 0x180018u, 0x180018u, 0x30000Cu,
        0x600006u, 0x600006u, 0xC00003u, 0xFFFFFFu, 0x7FFFFEu,
    };
    for (int r = 0; r < 20; r++) {
        uint32_t bits = rows[r];
        for (int c = 0; c < 24; c++) {
            if (bits & (1u << (23 - c))) {
                gfx_px(fb, x + c, y + r, 1);
            }
        }
    }
    gfx_rect(fb, x + 11, y + 6, 2, 6, 1);
    gfx_rect(fb, x + 11, y + 14, 2, 2, 1);
}

static void msg_icon_circle(uint8_t *fb, int x, int y)
{
    /* 20x20 8路严格对称平滑正圆环（匀称 1.6 px 环宽，消除顶底尖凸与锯齿）。 */
    static const uint32_t top10[10] = {
        0x01F80u, 0x07FE0u, 0x0E070u, 0x18018u, 0x3000Cu,
        0x60006u, 0x60006u, 0xC0003u, 0xC0003u, 0xC0003u,
    };
    for (int r = 0; r < 10; r++) {
        uint32_t row = top10[r];
        for (int c = 0; c < 20; c++) {
            if (row & (1u << (19 - c))) {
                gfx_px(fb, x + c, y + r, 1);
                gfx_px(fb, x + c, y + 19 - r, 1);
            }
        }
    }
}

static void msg_icon_alert(uint8_t *fb, int x, int y)
{
    msg_icon_circle(fb, x, y);
    gfx_rect(fb, x + 9, y + 5, 2, 6, 1);
    gfx_rect(fb, x + 9, y + 13, 2, 2, 1);
}

static void msg_icon_check(uint8_t *fb, int x, int y)
{
    msg_icon_circle(fb, x, y);
    for (int t = 0; t < 4; t++) {
        gfx_px(fb, x + 5 + t, y + 10 + t, 1);
        gfx_px(fb, x + 5 + t, y + 11 + t, 1);
    }
    for (int t = 0; t < 7; t++) {
        gfx_px(fb, x + 8 + t, y + 13 - t, 1);
        gfx_px(fb, x + 8 + t, y + 14 - t, 1);
    }
}

void mp_ui_msg(uint8_t *fb, const mp_ui_state_t *st)
{
    if (!fb || !st) {
        return;
    }

    uint8_t n = st->msg_lines;
    if (n > 3) {
        n = 3;
    }

    enum {
        ICON_NONE = 0,
        ICON_BOOK,
        ICON_WARN,
        ICON_ALERT,
        ICON_CHECK
    } kind = ICON_NONE;

    if (st->screen == MP_SCREEN_EMPTY) {
        kind = ICON_BOOK;
    } else if (st->screen == MP_SCREEN_ERROR) {
        kind = ICON_WARN;
    } else if (st->screen == MP_SCREEN_RESTORE) {
        kind = st->hide_hints ? ICON_CHECK : ICON_ALERT;
    } else if (st->screen == MP_SCREEN_DELETE ||
               st->screen == MP_SCREEN_NO_LOCK_IMG) {
        kind = ICON_ALERT;
    } else if (st->screen == MP_SCREEN_TIME_DONE) {
        kind = ICON_CHECK;
    }

    const int avail_top = MP_RULE_Y + 1;
    const int avail_h = st->hide_hints ? (MP_FB_H - avail_top)
                                       : (MP_HINT_TOP - avail_top);
    const int text_h = (n > 0) ? ((int)(n - 1) * MP_MSG_STEP + UI_TEXT_H) : 0;
    int top;

    if (kind != ICON_NONE && n > 0) {
        const int icon_h = 20;
        const int icon_gap = 6;
        int total_h = icon_h + icon_gap + text_h;
        int block_top = avail_top + (avail_h - total_h) / 2;
        if (kind == ICON_BOOK) {
            msg_icon_book(fb, (MP_FB_W - 24) / 2, block_top);
        } else if (kind == ICON_WARN) {
            msg_icon_warn(fb, (MP_FB_W - 24) / 2, block_top);
        } else if (kind == ICON_ALERT) {
            msg_icon_alert(fb, (MP_FB_W - 20) / 2, block_top);
        } else {
            msg_icon_check(fb, (MP_FB_W - 20) / 2, block_top);
        }
        top = block_top + icon_h + icon_gap;
    } else {
        top = avail_top + (avail_h - text_h) / 2;
    }

    for (uint8_t i = 0; i < n; i++) {
        if (st->msg_line[i].text) {
            mp_ui_text_center(fb, top + (int)i * MP_MSG_STEP,
                              st->msg_line[i].text);
        }
    }
}

/* ------------------------------------------------------------------ *
 * BOOK_INFO
 * ------------------------------------------------------------------ */
/* BOOK_INFO 元信息行：用 '\t' 分段，首段左对齐、末段右对齐、中段居中。 */
static void meta_line(uint8_t *fb, int top, int max_w, const char *s)
{
    char part[3][64];
    int n = 0;
    const char *p = s;

    while (p && *p && n < 3) {
        const char *tab = strchr(p, '\t');
        size_t len = tab ? (size_t)(tab - p) : strlen(p);
        if (len >= sizeof part[0]) {
            len = sizeof part[0] - 1;
        }
        memcpy(part[n], p, len);
        part[n][len] = '\0';
        n++;
        p = tab ? tab + 1 : NULL;
    }
    if (n == 0) {
        return;
    }

    int widths[3] = {0, 0, 0};
    for (int i = 0; i < n; i++) {
        widths[i] = ui_text_width(part[i]);
    }

    int left_x = MP_CONTENT_X;
    int right_edge = MP_CONTENT_X + max_w;
    if (n == 1) {
        ui_text_color(fb, left_x, top, part[0], max_w, 1);
        return;
    }

    ui_text_color(fb, left_x, top, part[0], max_w, 1);
    int last_x = right_edge - widths[n - 1];
    int min_last = left_x + widths[0] + 4;
    if (last_x < min_last) {
        last_x = min_last;
    }
    if (n == 3) {
        int l_end = left_x + widths[0];
        int mid_x = l_end + ((last_x - l_end) - widths[1]) / 2;
        if (mid_x < l_end + 2) {
            mid_x = l_end + 2;
        }
        if (mid_x < right_edge) {
            ui_text_color(fb, mid_x, top, part[1], right_edge - mid_x, 1);
        }
    }
    if (last_x < right_edge) {
        ui_text_color(fb, last_x, top, part[n - 1], right_edge - last_x, 1);
    }
}

void mp_ui_book_info(uint8_t *fb, const mp_ui_state_t *st)
{
    if (!fb || !st) {
        return;
    }

    if (st->meta.text) {
        meta_line(fb, MP_INFO_META_TOP, MP_CONTENT_W, st->meta.text);
    }

    ui_dotted_hline(fb, MP_CONTENT_X, MP_CONTENT_X + MP_CONTENT_W - 1,
                    MP_INFO_RULE_Y);

    uint8_t n = st->row_count;
    if (n > MP_INFO_ACT_ROWS) {
        n = MP_INFO_ACT_ROWS;
    }
    for (uint8_t i = 0; i < n; i++) {
        int col = (int)i % 2;
        int row = (int)i / 2;
        int x = MP_CONTENT_X + col * (MP_INFO_BTN_W + MP_INFO_BTN_GAP_X);
        int y = MP_INFO_BTN_TOP + row * (MP_INFO_BTN_H + MP_INFO_BTN_GAP_Y);
        int selected = ((int)st->sel == (int)i);
        int fg = selected ? 0 : 1;

        if (selected) {
            ui_rrect_fill(fb, x, y, MP_INFO_BTN_W, MP_INFO_BTN_H, 1);
        } else {
            ui_rrect_frame(fb, x, y, MP_INFO_BTN_W, MP_INFO_BTN_H, 1);
        }
        if (st->row[i].label) {
            int w = ui_text_width(st->row[i].label);
            int tx = x + (MP_INFO_BTN_W - w) / 2;
            if (tx < x + 2) {
                tx = x + 2;
            }
            ui_text_color(fb, tx, y + 3, st->row[i].label,
                          MP_INFO_BTN_W - 4, fg);
        }
    }
}

/* ------------------------------------------------------------------ *
 * JUMP / TIME_SET
 * ------------------------------------------------------------------ */
static void jump_knob(uint8_t *fb, int cx, int cy)
{
    /* 外圈 1 px 透气白光晕 + 11x11 黑圆环 + 5x5 白圆心。 */
    for (int dy = -6; dy <= 6; dy++) {
        for (int dx = -6; dx <= 6; dx++) {
            int d2 = dx * dx + dy * dy;
            if (d2 <= 38 && d2 > 27) {
                gfx_px(fb, cx + dx, cy + dy, 0);
            } else if (d2 <= 27 && d2 > 5) {
                gfx_px(fb, cx + dx, cy + dy, 1);
            } else if (d2 <= 5) {
                gfx_px(fb, cx + dx, cy + dy, 0);
            }
        }
    }
}

void mp_ui_jump(uint8_t *fb, const mp_ui_state_t *st)
{
    if (!fb || !st) {
        return;
    }

    char num[8];
    unsigned pct = st->jump_pct > 100 ? 100u : st->jump_pct;
    snprintf(num, sizeof num, "%u", pct);

    /* 上方居中显示 26 px 中号数字 + 16 px 精巧 '%'，底基线严格齐平。 */
    int nw = mp_gfx_width(&Inter_Num_26, num);
    int pw = mp_gfx_width(&Inter_UI_16, "%");
    int gap = 2;
    int total = nw + gap + pw;
    int tx = (MP_FB_W - total) / 2;
    mp_gfx_text(fb, &Inter_Num_26, ui_max(tx, 0), MP_JUMP_VAL_BASE, num);
    mp_gfx_text(fb, &Inter_UI_16, ui_max(tx, 0) + nw + gap, MP_JUMP_VAL_BASE,
                "%");

    /* 中间 200 px 精致进度滑块轨道 + 五等分刻度 + 圆形滑块把手。 */
    const int x0 = MP_JUMP_TRACK_X;
    const int x1 = MP_JUMP_TRACK_X + MP_JUMP_TRACK_W - 1;
    const int ty = MP_JUMP_TRACK_Y;

    gfx_hline(fb, x0, x1, ty, 1);
    for (int q = 0; q <= 4; q++) {
        int tx_tick = x0 + ((MP_JUMP_TRACK_W - 1) * q) / 4;
        gfx_vline(fb, tx_tick, ty + 2, ty + 4, 1);
    }

    int kx = x0 + ((MP_JUMP_TRACK_W - 1) * (int)pct + 50) / 100;
    if (kx > x0) {
        gfx_rect(fb, x0, ty - 1, kx - x0, 3, 1);
        gfx_px(fb, x0, ty - 1, 0);
        gfx_px(fb, x0, ty + 1, 0);
    }
    jump_knob(fb, kx, ty);

    /* 下方 5x7 刻度标注：0% / 50% / 100%。 */
    (void)ui_number5x7_opt(fb, x0, MP_JUMP_LABEL_Y, 0, 1, 1);
    (void)ui_number5x7_opt(fb, x0 + (MP_JUMP_TRACK_W - ui_pct5x7_w(50)) / 2,
                           MP_JUMP_LABEL_Y, 50, 1, 1);
    (void)ui_number5x7_opt(fb, x1 + 1 - ui_pct5x7_w(100), MP_JUMP_LABEL_Y, 100,
                           1, 1);
}

static void time_field(uint8_t *fb, int x, int y, int w, const char *text,
                       int selected)
{
    if (selected) {
        ui_rrect_fill(fb, x, y, w, MP_TIME_BOX_H, 1);
    }
    mp_gfx_text_color(fb, &Inter_Num_26, x + MP_TIME_PAD_X, y + 23, text,
                      selected ? 0 : 1);
}

static void time_sep(uint8_t *fb, int slot_x, int y, const char *sep)
{
    int sw = mp_gfx_width(&Inter_Num_26, sep);
    mp_gfx_text_color(fb, &Inter_Num_26, slot_x + (MP_TIME_SEP_W - sw) / 2,
                      y + 23, sep, 1);
}

void mp_ui_time_set(uint8_t *fb, const mp_ui_state_t *st)
{
    if (!fb || !st) {
        return;
    }

    int step = (int)(st->time_step % MP_TIME_FIELDS);
    char s_year[8];
    char s_mon[4];
    char s_day[4];
    char s_hour[4];
    char s_min[4];
    snprintf(s_year, sizeof s_year, "%04u", (unsigned)(st->year % 10000u));
    snprintf(s_mon, sizeof s_mon, "%02u", (unsigned)(st->month % 13u));
    snprintf(s_day, sizeof s_day, "%02u", (unsigned)(st->day % 32u));
    snprintf(s_hour, sizeof s_hour, "%02u", (unsigned)(st->hour % 24u));
    snprintf(s_min, sizeof s_min, "%02u", (unsigned)(st->min % 60u));

    /* 上行完整日期：YYYY - MM - DD（x=25 起，总宽 200 px 严格居中）。 */
    int dx = MP_TIME_DATE_X;
    time_field(fb, dx, MP_TIME_DATE_Y, MP_TIME_YEAR_W, s_year, step == 0);
    dx += MP_TIME_YEAR_W;
    time_sep(fb, dx, MP_TIME_DATE_Y, "-");
    dx += MP_TIME_SEP_W;
    time_field(fb, dx, MP_TIME_DATE_Y, MP_TIME_TWO_W, s_mon, step == 1);
    dx += MP_TIME_TWO_W;
    time_sep(fb, dx, MP_TIME_DATE_Y, "-");
    dx += MP_TIME_SEP_W;
    time_field(fb, dx, MP_TIME_DATE_Y, MP_TIME_TWO_W, s_day, step == 2);

    /* 下行完整时间：HH : MM（x=73 起，总宽 104 px 严格居中）。 */
    int cx = MP_TIME_CLOCK_X;
    time_field(fb, cx, MP_TIME_CLOCK_Y, MP_TIME_TWO_W, s_hour, step == 3);
    cx += MP_TIME_TWO_W;
    time_sep(fb, cx, MP_TIME_CLOCK_Y, ":");
    cx += MP_TIME_SEP_W;
    time_field(fb, cx, MP_TIME_CLOCK_Y, MP_TIME_TWO_W, s_min, step == 4);
}

/* ------------------------------------------------------------------ *
 * 背光设置页 / 快捷背光浮窗 / 电池空自动关机提示页
 * ------------------------------------------------------------------ */
static void sun_icon15(uint8_t *fb, int x, int y)
{
    /* 15x15 对称太阳图标（8 条光芒 + 7x7 圆环中心）。 */
    static const uint16_t rows[15] = {
        0x0080u, 0x0080u, 0x1084u, 0x0808u, 0x03E0u,
        0x0630u, 0x0410u, 0x7417u, 0x0410u, 0x0630u,
        0x03E0u, 0x0808u, 0x1084u, 0x0080u, 0x0080u,
    };
    for (int r = 0; r < 15; r++) {
        uint16_t bits = rows[r];
        for (int c = 0; c < 15; c++) {
            if (bits & (uint16_t)(0x4000u >> c)) {
                gfx_px(fb, x + c, y + r, 1);
            }
        }
    }
}

/* 4 档（0..3）水平滑条，复刻 jump_knob 圆滑把手风格。 */
static void bl_slider(uint8_t *fb, int x0, int track_w, int cy, uint8_t level)
{
    uint8_t lv = level > 3 ? 3 : level;
    int x1 = x0 + track_w - 1;

    gfx_hline(fb, x0, x1, cy, 1);
    for (int q = 0; q <= 3; q++) {
        int tx = x0 + ((track_w - 1) * q + 1) / 3;
        gfx_vline(fb, tx, cy - 3, cy - 2, 1);
        gfx_vline(fb, tx, cy + 2, cy + 3, 1);
    }

    int kx = x0 + ((track_w - 1) * (int)lv + 1) / 3;
    if (kx > x0) {
        gfx_rect(fb, x0, cy - 1, kx - x0, 3, 1);
        gfx_px(fb, x0, cy - 1, 0);
        gfx_px(fb, x0, cy + 1, 0);
    }
    jump_knob(fb, kx, cy);
}

void mp_ui_backlight(uint8_t *fb, const mp_ui_state_t *st)
{
    if (!fb || !st) {
        return;
    }
    uint8_t lv = st->bl_level > 3 ? 3 : st->bl_level;
    const char *lv_str = k_bl_level_names[lv];

    /* 1) 上行居中：太阳图标 + 档位文字。 */
    int lw = mp_ui_text_w(lv_str);
    int head_w = 15 + 6 + lw;
    int hx = (MP_FB_W - head_w) / 2;
    sun_icon15(fb, hx, 27);
    mp_ui_text(fb, hx + 15 + 6, 26, lv_str, lw);

    /* 2) 中部：4 档滑条（x=35..214）。 */
    bl_slider(fb, 35, 180, 56, lv);

    /* 3) 下方居中提示：长按 [▭] 快速调整背光（文字顶 y=77，键帽顶 y=78，两侧留 3 px 呼吸间隙）。 */
    const int key_gap = 3;
    int w_pre = mp_ui_text_w(TXT_BL_QUICK_PREFIX);
    int w_suf = mp_ui_text_w(TXT_BL_QUICK_SUFFIX);
    int tip_w = w_pre + key_gap + MP_KEYCAP_W + key_gap + w_suf;
    int tx = (MP_FB_W - tip_w) / 2;
    if (tx < 2) {
        tx = 2;
    }
    mp_ui_text(fb, tx, 77, TXT_BL_QUICK_PREFIX, w_pre);
    tx += w_pre + key_gap;
    mp_hints_keycap(fb, MP_HINT_CAPSULE, tx, 78);
    tx += MP_KEYCAP_W + key_gap;
    mp_ui_text(fb, tx, 77, TXT_BL_QUICK_SUFFIX, w_suf);
}

void mp_ui_backlight_popup(uint8_t *fb, const mp_ui_state_t *st)
{
    if (!fb || !st) {
        return;
    }
    uint8_t lv = st->bl_level > 3 ? 3 : st->bl_level;
    const char *lv_str = k_bl_level_names[lv];

    /* 外围 2 px 白边留空 + 194x78 2 px 粗微圆角边框（完全居中于内容区）。 */
    ui_rrect_fill(fb, 26, 20, 198, 82, 0);
    ui_rrect_frame(fb, 28, 22, 194, 78, 1);
    ui_rrect_frame(fb, 29, 23, 192, 76, 1);

    /* 标题行（y=29）：太阳图标 + "背光 2档"。 */
    char title[24];
    snprintf(title, sizeof(title), "%s %s", TXT_BL_LABEL, lv_str);
    int tw = mp_ui_text_w(title);
    int head_w = 15 + 5 + tw;
    int hx = (MP_FB_W - head_w) / 2;
    sun_icon15(fb, hx, 30);
    mp_ui_text(fb, hx + 15 + 5, 29, title, tw);

    /* 滑条（x=46..203, cy=55）。 */
    bl_slider(fb, 46, 158, 55, lv);

    /* 点线分隔 + 内嵌键帽提示：[◀][▶]调整  [○]确定。 */
    ui_dotted_hline(fb, 38, 211, 70);
    int w_adj = mp_ui_text_w(TXT_BL_ADJUST);
    int w_ok = mp_ui_text_w(TXT_BL_CONFIRM);
    int g1 = 10 + MP_KEYCAP_W + MP_KEYCAP_GAP + w_adj;
    int g2 = MP_KEYCAP_W + MP_KEYCAP_GAP + w_ok;
    const int gap = 14;
    int px = (MP_FB_W - (g1 + gap + g2)) / 2;

    mp_hints_keycap(fb, MP_HINT_PREV, px, 77);
    mp_hints_keycap(fb, MP_HINT_NEXT, px + 10, 77);
    mp_ui_text(fb, px + 10 + MP_KEYCAP_W + MP_KEYCAP_GAP, 76, TXT_BL_ADJUST, w_adj);

    px += g1 + gap;
    mp_hints_keycap(fb, MP_HINT_OK, px, 77);
    mp_ui_text(fb, px + MP_KEYCAP_W + MP_KEYCAP_GAP, 76, TXT_BL_CONFIRM, w_ok);
}

static void low_batt_icon(uint8_t *fb, int x, int y)
{
    /* 52x26 空电池大图标：48x26 微圆角 2 px 粗外壳 + 4x10 正极帽 + 居中感叹号。 */
    ui_rrect_frame(fb, x, y, 48, 26, 1);
    ui_rrect_frame(fb, x + 1, y + 1, 46, 24, 1);
    gfx_px(fb, x + 2, y + 2, 1);
    gfx_px(fb, x + 45, y + 2, 1);
    gfx_px(fb, x + 2, y + 23, 1);
    gfx_px(fb, x + 45, y + 23, 1);

    /* 右侧正极帽（4x10，右端削 1 px 圆角）。 */
    gfx_rect(fb, x + 48, y + 8, 3, 10, 1);
    gfx_vline(fb, x + 51, y + 9, y + 16, 1);

    /* 内部居中圆头感叹号：上竖杠 4x10 + 下圆点 4x4。 */
    ui_rrect_fill(fb, x + 22, y + 5, 4, 10, 1);
    ui_rrect_fill(fb, x + 22, y + 17, 4, 4, 1);
}

void mp_ui_low_batt(uint8_t *fb)
{
    if (!fb) {
        return;
    }
    low_batt_icon(fb, (MP_FB_W - 52) / 2, 25);
    mp_ui_text_center(fb, 61, TXT_LOW_BATT_1);
    mp_ui_text_center(fb, 81, TXT_LOW_BATT_2);
}

/* ------------------------------------------------------------------ *
 * LOCK
 * ------------------------------------------------------------------ */
static void lock_image(uint8_t *fb, const uint8_t *img)
{
    for (int y = 0; y < MP_FB_H; y++) {
        int sy = y + 6;
        const uint8_t *row = img + (size_t)(sy >> 3) * (size_t)MP_FB_W;
        uint8_t mask = (uint8_t)(1u << (sy & 7));
        for (int x = 0; x < MP_FB_W; x++) {
            gfx_px(fb, x, y, (row[x] & mask) ? 1 : 0);
        }
    }
}

static void lock_icon(uint8_t *fb, int x, int y)
{
    /* 20x22 圆弧锁梁 + 微圆角实心锁身 + 白色钥匙孔。 */
    gfx_hline(fb, x + 6, x + 13, y + 1, 1);
    gfx_hline(fb, x + 5, x + 14, y + 2, 1);
    gfx_rect(fb, x + 4, y + 3, 2, 6, 1);
    gfx_rect(fb, x + 14, y + 3, 2, 6, 1);
    ui_rrect_fill(fb, x + 1, y + 9, 18, 12, 1);
    gfx_rect(fb, x + 8, y + 12, 4, 2, 0);
    gfx_rect(fb, x + 9, y + 14, 2, 3, 0);
}

static void lock_icon_sm(uint8_t *fb, int x, int y)
{
    /* 12x14 迷你锁头：圆弧 1 px 锁梁 + 微圆角实心锁身 + 白色钥匙孔。 */
    gfx_hline(fb, x + 4, x + 7, y, 1);
    gfx_px(fb, x + 3, y + 1, 1);
    gfx_px(fb, x + 8, y + 1, 1);
    gfx_vline(fb, x + 3, y + 2, y + 5, 1);
    gfx_vline(fb, x + 8, y + 2, y + 5, 1);
    ui_rrect_fill(fb, x, y + 6, 12, 8, 1);
    gfx_rect(fb, x + 5, y + 8, 2, 3, 0);
}

static void lock_hint(uint8_t *fb)
{
    int w_hold = mp_ui_text_w(TXT_HOLD);
    int w_unlock = mp_ui_text_w(TXT_UNLOCK);
    int total = w_hold + MP_KEYCAP_GAP + MP_KEYCAP_W + MP_KEYCAP_GAP + w_unlock;
    int x = (MP_FB_W - total) / 2;

    mp_ui_text(fb, x, MP_LOCK_HINT_TOP, TXT_HOLD, w_hold);
    x += w_hold + MP_KEYCAP_GAP;
    mp_hints_keycap(fb, MP_HINT_OK, x, MP_KEYCAP_TOP);
    x += MP_KEYCAP_W + MP_KEYCAP_GAP;
    mp_ui_text(fb, x, MP_LOCK_HINT_TOP, TXT_UNLOCK, w_unlock);
}

static void format_stat_num(uint32_t v, char *num, size_t cap,
                            const char **suffix)
{
    if (v >= 10000000u) {
        snprintf(num, cap, "%u", (unsigned)(v / 10000u));
        *suffix = TXT_WAN;
    } else if (v >= 10000u) {
        snprintf(num, cap, "%u.%u", (unsigned)(v / 10000u),
                 (unsigned)((v % 10000u) / 1000u));
        *suffix = TXT_WAN;
    } else {
        snprintf(num, cap, "%u", (unsigned)v);
        *suffix = NULL;
    }
}

static void lock_stat_col(uint8_t *fb, int cx, const char *num_str,
                          const char *suffix_utf8, const char *label_utf8)
{
    int nw = mp_gfx_width(&Inter_Num_26, num_str);
    int sw = suffix_utf8 ? mp_ui_text_w(suffix_utf8) : 0;
    int gap = suffix_utf8 ? 2 : 0;
    int total = nw + gap + sw;
    int vx = cx - total / 2;

    /* 26 px 中号数字与 16 px 中文单位底缘齐平于 y=62。 */
    mp_gfx_text(fb, &Inter_Num_26, ui_max(vx, 2), 63, num_str);
    if (suffix_utf8) {
        mp_ui_text(fb, ui_max(vx, 2) + nw + gap, 47, suffix_utf8, sw);
    }

    int lw = mp_ui_text_w(label_utf8);
    int lx = cx - lw / 2;
    mp_ui_text(fb, ui_max(lx, 2), 70, label_utf8, lw);
}

static void lock_stats_screen(uint8_t *fb, const mp_ui_state_t *st)
{
    /* 顶部居中提示：12x14 迷你锁头 + 已锁定。 */
    const int icon_w = 12;
    int tw = mp_ui_text_w(TXT_LOCKED);
    int top_total = icon_w + 5 + tw;
    int top_x = (MP_FB_W - top_total) / 2;
    lock_icon_sm(fb, top_x, 11);
    mp_ui_text(fb, top_x + icon_w + 5, 10, TXT_LOCKED, tw);

    /* 上下轻量点线与中轴竖点线框出双栏阅读统计区。 */
    ui_dotted_hline(fb, 18, 231, 31);
    for (int y = 43; y <= 85; y += 3) {
        gfx_px(fb, 125, y, 1);
    }

    char num_mins[16];
    char num_chars[16];
    const char *suf_mins = NULL;
    const char *suf_chars = NULL;
    format_stat_num(st->read_mins, num_mins, sizeof num_mins, &suf_mins);
    format_stat_num(st->read_chars, num_chars, sizeof num_chars, &suf_chars);

    lock_stat_col(fb, 66, num_mins, suf_mins, TXT_READ_MINS);
    lock_stat_col(fb, 184, num_chars, suf_chars, TXT_READ_CHARS);

    ui_dotted_hline(fb, 18, 231, 95);
    lock_hint(fb);
}

void mp_ui_lock(uint8_t *fb, const mp_ui_state_t *st)
{
    if (!fb || !st) {
        return;
    }

    if (st->lock_mode == MP_LOCK_MODE_IMAGE && st->lock_valid && st->lock_img) {
        lock_image(fb, st->lock_img);
        return;
    }
    if (st->lock_mode == MP_LOCK_MODE_PROMPT) {
        lock_icon(fb, MP_LOCK_ICON_X, MP_LOCK_ICON_Y);
        mp_ui_text_center(fb, MP_LOCK_TEXT_TOP, TXT_LOCKED);
        lock_hint(fb);
        return;
    }

    lock_stats_screen(fb, st);
}

/* ------------------------------------------------------------------ *
 * 文本工具
 * ------------------------------------------------------------------ */
int mp_ui_text_w(const char *utf8)
{
    return ui_text_width(utf8);
}

void mp_ui_text_color(uint8_t *fb, int x, int top, const char *utf8, int max_w,
                      int black)
{
    if (!fb || !utf8) {
        return;
    }
    int avail = MP_FB_W - x;
    if (max_w <= 0 || max_w > avail) {
        max_w = avail;
    }
    ui_text_color(fb, x, top, utf8, max_w, black);
}

void mp_ui_text(uint8_t *fb, int x, int top, const char *utf8, int max_w)
{
    mp_ui_text_color(fb, x, top, utf8, max_w, 1);
}

void mp_ui_text_center(uint8_t *fb, int top, const char *utf8)
{
    if (!fb || !utf8) {
        return;
    }
    int w = mp_ui_text_w(utf8);
    int x = (MP_FB_W - w) / 2;
    mp_ui_text(fb, ui_max(x, 0), top, utf8, MP_FB_W - ui_max(x, 0));
}

void mp_ui_gb_text_color(uint8_t *fb, int x, int top, const char *s, uint8_t enc,
                         int max_w, int black)
{
    if (!fb || !s) {
        return;
    }
    int avail = MP_FB_W - x;
    if (max_w <= 0 || max_w > avail) {
        max_w = avail;
    }
    if (max_w <= 0) {
        return;
    }
    /* ASCII(Dialog_plain_16) 用 top+13 使字母与 Inter_UI_16 齐平；
     * GB2312(HZK16) 内部按 baseline-12 定位字形顶，必须保持 top+12。 */
    int base = (enc == MP_ENC_ASCII) ? (top + 13) : (top + 12);
    mp_text_draw_color(fb, s, enc, x, base, max_w, black);
}

void mp_ui_gb_text(uint8_t *fb, int x, int top, const char *s, uint8_t enc,
                   int max_w)
{
    mp_ui_gb_text_color(fb, x, top, s, enc, max_w, 1);
}

void mp_ui_underline(uint8_t *fb, int row_top, int text_w)
{
    if (!fb) {
        return;
    }
    int w = ui_min(text_w, MP_CONTENT_W);
    int x = MP_CONTENT_X + (w - MP_SEL_RULE_W) / 2;
    if (x < MP_CONTENT_X) {
        x = MP_CONTENT_X;
    }
    gfx_rect(fb, x, row_top + MP_SEL_RULE_DY, MP_SEL_RULE_W, MP_SEL_RULE_H,
             1);
}

/* ------------------------------------------------------------------ *
 * 入口
 * ------------------------------------------------------------------ */
void mp_ui_clear(uint8_t *fb)
{
    gfx_clear(fb, 0);
}

void mp_ui_render(const mp_ui_state_t *st, uint8_t *fb)
{
    if (!fb) {
        return;
    }
    mp_ui_clear(fb);
    if (!st) {
        return;
    }

    if (st->screen == MP_SCREEN_LOCK) {
        mp_ui_lock(fb, st);
        return;
    }
    if (st->screen == MP_SCREEN_LOW_BATT) {
        mp_ui_low_batt(fb);
        return;
    }

    mp_ui_status_bar(fb, st);

    switch ((mp_screen_t)st->screen) {
    case MP_SCREEN_CLOCK:
        mp_ui_clock(fb, st);
        break;
    case MP_SCREEN_SELECT:
        mp_ui_list(fb, st);
        break;
    case MP_SCREEN_SETTINGS:
        mp_ui_settings(fb, st);
        break;
    case MP_SCREEN_READ:
        mp_ui_read(fb, st);
        break;
    case MP_SCREEN_ABOUT:
        mp_ui_about(fb, st);
        break;
    case MP_SCREEN_BOOK_INFO:
        mp_ui_book_info(fb, st);
        break;
    case MP_SCREEN_JUMP:
        mp_ui_jump(fb, st);
        break;
    case MP_SCREEN_TIME_SET:
        mp_ui_time_set(fb, st);
        break;
    case MP_SCREEN_BACKLIGHT:
        mp_ui_backlight(fb, st);
        break;
    case MP_SCREEN_EMPTY:
    case MP_SCREEN_ERROR:
    case MP_SCREEN_RESTORE:
    case MP_SCREEN_DELETE:
    case MP_SCREEN_TIME_DONE:
    case MP_SCREEN_NO_LOCK_IMG:
        mp_ui_msg(fb, st);
        break;
    default:
        break;
    }

    if (!st->hide_hints) {
        mp_hint_set_t hints;
        mp_hints_for((mp_screen_t)st->screen, &hints);
        mp_hints_draw(fb, &hints);
    }

    if (st->bl_popup) {
        mp_ui_backlight_popup(fb, st);
    }
}
