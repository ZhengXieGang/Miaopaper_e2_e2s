/* mp_hints.c — 底栏键帽与逐屏提示（纯 C：不碰 ESP-IDF、不读文件）。
 *
 * 形制来自原厂 1.3.739 的"中文动词 + 键帽图标"（stock_ui.md §4.1/§4.2），
 * 本工程冻结为 docs/PORT_SPEC.md §1.5.4/§1.7.1/§2.5：
 *   - 键帽放在 14x14 布局槽里，**没有外框**（UI v2）；
 *   - 一组 = [前缀中文][键帽 1..2 个][后缀中文]，组内间距 2，组间 12；
 *     两组屏 x=30/144，三组屏 x=30/106/166，单组屏居中；
 *   - 文字顶 y=105，键帽 y=106..119；
 *   - 总宽 > 242 时从尾部丢整组，绝不换行（底栏只有 18 px 高）。
 *
 * 屏上不出现任何英文键名：键只有 5 个符号（◀ ▶ ○ × ↻），由下面
 * 冻结的 1bpp 位图给出。
 *
 * 谁调用：只有 mp_ui_render()（固件与 sim 都不要自己叠底栏）。
 * 逐字文案真源：docs/COPY_DECK.md §4。
 */
#include "mp_hints.h"

#include <string.h>

#include "gfx.h"

/* 位图按 14 行 x 2 B 冻结：若常量被改，这里先编译失败。 */
_Static_assert(MP_KEYCAP_W == 14, "keycap art is 14 px wide");
_Static_assert(MP_KEYCAP_H == 14, "keycap art is 14 rows");

/* ------------------------------------------------------------------ *
 * 键帽位图（14 行 x 2 B，MSB 最左，bit=1 墨）
 *
 * 2026-11-25 评审：去掉所有键帽的 14x14 方框，只留符号本身（原厂键帽就是
 * 独立 1bpp 符号，见 stock_ui.md §1/§4.1）。返回/取消键帽改为 ×（匹配硬件
 * 右侧 × 键）；FUNC 用刷新箭头。
 * ------------------------------------------------------------------ */
const uint8_t mp_keycap_bits[MP_HINT_KEY_COUNT][MP_KEYCAP_H * 2] = {
    /* ◀ PREV（7x14 满高实心等腰左指三角，与 16 px 底部文字视觉等高） */
    [MP_HINT_PREV] = {
        0x00, 0x20,   /* ..........#... */
        0x00, 0x60,   /* .........##... */
        0x00, 0xE0,   /* ........###... */
        0x01, 0xE0,   /* .......####... */
        0x03, 0xE0,   /* ......#####... */
        0x07, 0xE0,   /* .....######... */
        0x0F, 0xE0,   /* ....#######... */
        0x0F, 0xE0,   /* ....#######... */
        0x07, 0xE0,   /* .....######... */
        0x03, 0xE0,   /* ......#####... */
        0x01, 0xE0,   /* .......####... */
        0x00, 0xE0,   /* ........###... */
        0x00, 0x60,   /* .........##... */
        0x00, 0x20,   /* ..........#... */
    },
    /* ▶ NEXT（与 ◀ 严格左右镜像，双键步进 10 px 时两三角间留 2 px） */
    [MP_HINT_NEXT] = {
        0x10, 0x00,   /* ...#.......... */
        0x18, 0x00,   /* ...##......... */
        0x1C, 0x00,   /* ...###........ */
        0x1E, 0x00,   /* ...####....... */
        0x1F, 0x00,   /* ...#####...... */
        0x1F, 0x80,   /* ...######..... */
        0x1F, 0xC0,   /* ...#######.... */
        0x1F, 0xC0,   /* ...#######.... */
        0x1F, 0x80,   /* ...######..... */
        0x1F, 0x00,   /* ...#####...... */
        0x1E, 0x00,   /* ...####....... */
        0x1C, 0x00,   /* ...###........ */
        0x18, 0x00,   /* ...##......... */
        0x10, 0x00,   /* ...#.......... */
    },
    /* ○ OK（14x14 8路严格对称 1 px 正圆环，与汉字笔画和高度协调） */
    [MP_HINT_OK] = {
        0x0F, 0xC0,   /* ....######.... */
        0x30, 0x30,   /* ..##......##.. */
        0x40, 0x08,   /* .#..........#. */
        0x40, 0x08,   /* .#..........#. */
        0x80, 0x04,   /* #............# */
        0x80, 0x04,   /* #............# */
        0x80, 0x04,   /* #............# */
        0x80, 0x04,   /* #............# */
        0x80, 0x04,   /* #............# */
        0x80, 0x04,   /* #............# */
        0x40, 0x08,   /* .#..........#. */
        0x40, 0x08,   /* .#..........#. */
        0x30, 0x30,   /* ..##......##.. */
        0x0F, 0xC0,   /* ....######.... */
    },
    /* × 返回/取消（12x12 居中对称斜叉，光学体量与 14x14 圆环一致） */
    [MP_HINT_MENU] = {
        0x00, 0x00,   /* .............. */
        0x20, 0x10,   /* ..#........#.. */
        0x60, 0x18,   /* .##........##. */
        0x30, 0x30,   /* ..##......##.. */
        0x18, 0x60,   /* ...##....##... */
        0x0C, 0xC0,   /* ....##..##.... */
        0x07, 0x80,   /* .....####..... */
        0x07, 0x80,   /* .....####..... */
        0x0C, 0xC0,   /* ....##..##.... */
        0x18, 0x60,   /* ...##....##... */
        0x30, 0x30,   /* ..##......##.. */
        0x60, 0x18,   /* .##........##. */
        0x20, 0x10,   /* ..#........#.. */
        0x00, 0x00,   /* .............. */
    },
    /* ▭ CAPSULE 胶囊菜单键（14x8 居中圆角胶囊，两端半圆弧饱满） */
    [MP_HINT_FUNC] = {
        0x00, 0x00,   /* .............. */
        0x00, 0x00,   /* .............. */
        0x00, 0x00,   /* .............. */
        0x1F, 0xE0,   /* ...########... */
        0x60, 0x18,   /* .##........##. */
        0x80, 0x04,   /* #............# */
        0x80, 0x04,   /* #............# */
        0x80, 0x04,   /* #............# */
        0x80, 0x04,   /* #............# */
        0x60, 0x18,   /* .##........##. */
        0x1F, 0xE0,   /* ...########... */
        0x00, 0x00,   /* .............. */
        0x00, 0x00,   /* .............. */
        0x00, 0x00,   /* .............. */
    },
};

/* ------------------------------------------------------------------ *
 * 文案（逐字来自 COPY_DECK §4）
 * ------------------------------------------------------------------ */
static const char TXT_SETTINGS[] = "设置";
static const char TXT_LIBRARY[] = "书库";
static const char TXT_SELECT[] = "选择";
static const char TXT_OPEN[] = "打开";
static const char TXT_CONFIRM[] = "确定";
static const char TXT_BACK[] = "返回";
static const char TXT_RETRY[] = "重试";
static const char TXT_REFRESH[] = "刷新";
static const char TXT_CANCEL[] = "取消";
static const char TXT_RESTORE[] = "恢复";
static const char TXT_HOLD[] = "长按";
static const char TXT_ADJUST[] = "调整";
static const char TXT_NEXT[] = "下一步";
static const char TXT_JUMP[] = "跳转";
static const char TXT_DELETE[] = "删除";

/* ------------------------------------------------------------------ *
 * 逐屏表（PORT_SPEC §2.5 冻结）
 * ------------------------------------------------------------------ */
static const mp_hint_item_t HINT_CLOCK[] = {
    {1, {MP_HINT_PREV, MP_HINT_PREV}, NULL, TXT_SETTINGS},
    {1, {MP_HINT_NEXT, MP_HINT_NEXT}, NULL, TXT_LIBRARY},
};

static const mp_hint_item_t HINT_SELECT[] = {
    {2, {MP_HINT_PREV, MP_HINT_NEXT}, NULL, TXT_SELECT},
    {1, {MP_HINT_OK, MP_HINT_OK}, NULL, TXT_OPEN},
    {1, {MP_HINT_MENU, MP_HINT_MENU}, NULL, TXT_BACK},
};

static const mp_hint_item_t HINT_SETTINGS[] = {
    {2, {MP_HINT_PREV, MP_HINT_NEXT}, NULL, TXT_SELECT},
    {1, {MP_HINT_OK, MP_HINT_OK}, NULL, TXT_CONFIRM},
    {1, {MP_HINT_MENU, MP_HINT_MENU}, NULL, TXT_BACK},
};

static const mp_hint_item_t HINT_ABOUT[] = {
    {1, {MP_HINT_MENU, MP_HINT_MENU}, NULL, TXT_BACK},
};

static const mp_hint_item_t HINT_BOOK_INFO[] = {
    {2, {MP_HINT_PREV, MP_HINT_NEXT}, NULL, TXT_SELECT},
    {1, {MP_HINT_OK, MP_HINT_OK}, NULL, TXT_CONFIRM},
    {1, {MP_HINT_MENU, MP_HINT_MENU}, NULL, TXT_BACK},
};

static const mp_hint_item_t HINT_JUMP[] = {
    {2, {MP_HINT_PREV, MP_HINT_NEXT}, NULL, TXT_ADJUST},
    {1, {MP_HINT_OK, MP_HINT_OK}, NULL, TXT_JUMP},
    {1, {MP_HINT_MENU, MP_HINT_MENU}, NULL, TXT_CANCEL},
};

static const mp_hint_item_t HINT_TIME_SET[] = {
    {2, {MP_HINT_PREV, MP_HINT_NEXT}, NULL, TXT_ADJUST},
    {1, {MP_HINT_OK, MP_HINT_OK}, NULL, TXT_NEXT},
    {1, {MP_HINT_MENU, MP_HINT_MENU}, NULL, TXT_CANCEL},
};

static const mp_hint_item_t HINT_DELETE[] = {
    {1, {MP_HINT_OK, MP_HINT_OK}, TXT_HOLD, TXT_DELETE},
    {1, {MP_HINT_MENU, MP_HINT_MENU}, NULL, TXT_CANCEL},
};

static const mp_hint_item_t HINT_BACKLIGHT[] = {
    {2, {MP_HINT_PREV, MP_HINT_NEXT}, NULL, TXT_ADJUST},
    {1, {MP_HINT_OK, MP_HINT_OK}, NULL, TXT_CONFIRM},
    {1, {MP_HINT_MENU, MP_HINT_MENU}, NULL, TXT_CANCEL},
};

/* 空页用 [○]刷新  [×]返回；错页用 [○]重试  [×]返回（全部对齐真实五键）。 */
static const mp_hint_item_t HINT_EMPTY[] = {
    {1, {MP_HINT_OK, MP_HINT_OK}, NULL, TXT_REFRESH},
    {1, {MP_HINT_MENU, MP_HINT_MENU}, NULL, TXT_BACK},
};

static const mp_hint_item_t HINT_ERROR[] = {
    {1, {MP_HINT_OK, MP_HINT_OK}, NULL, TXT_RETRY},
    {1, {MP_HINT_MENU, MP_HINT_MENU}, NULL, TXT_BACK},
};

/* 破坏性提交用具体动词（COPY_DECK §2.9/§4：长按[○]恢复）。 */
static const mp_hint_item_t HINT_RESTORE[] = {
    {1, {MP_HINT_OK, MP_HINT_OK}, TXT_HOLD, TXT_RESTORE},
    {1, {MP_HINT_MENU, MP_HINT_MENU}, NULL, TXT_CANCEL},
};

static void set_items(mp_hint_set_t *out, const mp_hint_item_t *items,
                      uint8_t n)
{
    if (n > MP_HINT_MAX) {
        n = MP_HINT_MAX;
    }
    out->n = n;
    for (uint8_t i = 0; i < n; i++) {
        out->item[i] = items[i];
    }
}

uint8_t mp_hints_for(mp_screen_t screen, mp_hint_set_t *out)
{
    if (!out) {
        return 0;
    }
    memset(out, 0, sizeof *out);

    switch (screen) {
    case MP_SCREEN_CLOCK:
        set_items(out, HINT_CLOCK, 2);
        break;
    case MP_SCREEN_SELECT:
        set_items(out, HINT_SELECT, 3);
        break;
    case MP_SCREEN_SETTINGS:
        set_items(out, HINT_SETTINGS, 3);
        break;
    case MP_SCREEN_ABOUT:
    case MP_SCREEN_NO_LOCK_IMG:
        set_items(out, HINT_ABOUT, 1);
        break;
    case MP_SCREEN_BOOK_INFO:
        set_items(out, HINT_BOOK_INFO, 3);
        break;
    case MP_SCREEN_JUMP:
        set_items(out, HINT_JUMP, 3);
        break;
    case MP_SCREEN_TIME_SET:
        set_items(out, HINT_TIME_SET, 3);
        break;
    case MP_SCREEN_DELETE:
        set_items(out, HINT_DELETE, 2);
        break;
    case MP_SCREEN_EMPTY:
        set_items(out, HINT_EMPTY, 2);
        break;
    case MP_SCREEN_ERROR:
        set_items(out, HINT_ERROR, 2);
        break;
    case MP_SCREEN_RESTORE:
        set_items(out, HINT_RESTORE, 2);
        break;
    case MP_SCREEN_BACKLIGHT:
        set_items(out, HINT_BACKLIGHT, 3);
        break;
    case MP_SCREEN_READ:      /* 底栏位置被 5 行正文 + 2 px 进度条占满 */
    case MP_SCREEN_TIME_DONE: /* 过渡页不叠提示 */
    case MP_SCREEN_LOCK:      /* 锁图不叠字；兜底页的提示由 mp_ui_lock 画 */
    case MP_SCREEN_LOW_BATT:  /* 电池空提示页不画底栏 */
    default:
        break;
    }
    return out->n;
}

/* ------------------------------------------------------------------ *
 * 排版（全部组按实际总宽水平居中对称排版）
 * ------------------------------------------------------------------ */
#define HINT_PAIR_STEP 10   /* 双键组内第二个键帽的步进（避免多余空隙） */
#define HINT_TWO_GAP 36     /* 两组屏居中时的默认组间距 */
#define HINT_MIN_GAP 8      /* 三组及以上的最小降级组间距 */

/* 一组的宽度：[pre] 2 [key1 (或 key1+10+key2)] 2 [post]。 */
static int item_width(const mp_hint_item_t *it)
{
    int nk = it->nkeys;
    int seg = 0;
    int w = 0;

    if (nk > 2) {
        nk = 2;
    }
    if (it->pre) {
        w += mp_ui_text_w(it->pre);
        seg++;
    }
    if (nk > 0) {
        if (seg) {
            w += MP_KEYCAP_GAP;
        }
        w += (nk == 1) ? MP_KEYCAP_W : (HINT_PAIR_STEP + MP_KEYCAP_W);
        seg++;
    }
    if (it->post) {
        if (seg) {
            w += MP_KEYCAP_GAP;
        }
        w += mp_ui_text_w(it->post);
    }
    return w;
}

/* 计算在 MP_HINT_MAX_W(=234) 内可排布的组数、组距、首组起点与各组宽度。 */
static int hint_layout(const mp_hint_set_t *s, int *out_count, int *out_gap,
                       int *out_start_x, int widths[MP_HINT_MAX])
{
    int n = (s && s->n > 0) ? (s->n > MP_HINT_MAX ? MP_HINT_MAX : s->n) : 0;
    int valid = 0;
    for (int i = 0; i < n; i++) {
        int gw = item_width(&s->item[i]);
        if (gw <= 0 || gw > MP_HINT_MAX_W) {
            break;
        }
        widths[valid++] = gw;
    }

    int count = valid;
    int gap = 0;
    while (count > 0) {
        int sum = 0;
        for (int i = 0; i < count; i++) {
            sum += widths[i];
        }
        if (count == 1) {
            gap = 0;
            break;
        }
        if (count == 2) {
            if (sum <= MP_HINT_MAX_W) {
                int avail = MP_HINT_MAX_W - sum;
                gap = avail < HINT_TWO_GAP ? avail : HINT_TWO_GAP;
                break;
            }
        } else {
            if (sum + (count - 1) * MP_HINT_GAP <= MP_HINT_MAX_W) {
                gap = MP_HINT_GAP;
                break;
            }
            if (sum + (count - 1) * HINT_MIN_GAP <= MP_HINT_MAX_W) {
                gap = HINT_MIN_GAP;
                break;
            }
        }
        count--;
    }

    int total = 0;
    for (int i = 0; i < count; i++) {
        total += widths[i];
    }
    if (count > 1) {
        total += (count - 1) * gap;
    }
    *out_count = count;
    *out_gap = gap;
    *out_start_x = (count > 0) ? (MP_FB_W - total) / 2 : MP_HINT_X;
    return total;
}

/* 实际横向跨度：首组左端 -> 末组右端。 */
int mp_hints_width(const mp_hint_set_t *s)
{
    if (!s || s->n == 0) {
        return 0;
    }
    int count = 0;
    int gap = 0;
    int start_x = 0;
    int widths[MP_HINT_MAX];
    return hint_layout(s, &count, &gap, &start_x, widths);
}

void mp_hints_keycap(uint8_t *fb, mp_hint_key_t key, int x, int ytop)
{
    if (!fb || (unsigned)key >= (unsigned)MP_HINT_KEY_COUNT) {
        return;
    }
    /* 1bpp 位图逐点贴入；gfx_blit1 内部按 250x122 裁剪。 */
    gfx_blit1(fb, x, ytop, MP_KEYCAP_W, MP_KEYCAP_H, mp_keycap_bits[key], 2);
}

void mp_hints_draw(uint8_t *fb, const mp_hint_set_t *s)
{
    if (!fb || !s || s->n == 0) {
        return;
    }

    int count = 0;
    int gap = 0;
    int x = 0;
    int widths[MP_HINT_MAX];
    hint_layout(s, &count, &gap, &x, widths);

    for (int i = 0; i < count; i++) {
        const mp_hint_item_t *it = &s->item[i];
        int gw = widths[i];
        int nk = it->nkeys;
        int seg = 0;

        if (i > 0) {
            x += gap;
        }
        if (nk > 2) {
            nk = 2;
        }

        if (it->pre) {
            mp_ui_text(fb, x, MP_HINT_TEXT_TOP, it->pre, gw);
            x += mp_ui_text_w(it->pre);
            seg++;
        }
        if (nk > 0) {
            if (seg) {
                x += MP_KEYCAP_GAP;
            }
            for (int k = 0; k < nk; k++) {
                mp_hints_keycap(fb, (mp_hint_key_t)it->key[k],
                                x + k * HINT_PAIR_STEP, MP_KEYCAP_TOP);
            }
            x += (nk == 1) ? MP_KEYCAP_W : (HINT_PAIR_STEP + MP_KEYCAP_W);
            seg++;
        }
        if (it->post) {
            if (seg) {
                x += MP_KEYCAP_GAP;
            }
            mp_ui_text(fb, x, MP_HINT_TEXT_TOP, it->post, gw);
            x += mp_ui_text_w(it->post);
        }
    }
}
