#pragma once
/* mp_hints.h — 底栏键帽提示（纯 C）。
 *
 * 原厂底栏 = 18 px 高、中文动词 + 1bpp 键帽符号。本工程沿用这个形制，但：
 *   - 绝不出现 MENU/OK/FUNC 这类英文键名；
 *   - 键帽是**没有方框的独立符号**（原厂就是独立位图）：◀ ▶ ○ × ↻，统一放在
 *     14x14 的布局槽里（× 对应机身右侧的返回/取消键）；
 *   - 只有"本屏真的可用"的键才出现，最多 4 组。
 *
 * 一组 = [前缀中文][键帽(1..2 个)][后缀中文]，例：
 *   "长按" + [○] + "解锁"      -> 长按[○]解锁
 *   [◀][▶] + "选择"           -> [◀][▶]选择
 *   [×] + "返回"               -> [×]返回
 * 版式（UI v3）：底栏全部组按实际总宽水平居中对称排版；双键组步进 10 px；
 *       组内元素间距 2 px，两组默认间距 36 px，三组默认间距 14 px；
 *       文字顶 y=105，键帽 106..119；总宽超出 MP_HINT_MAX_W(=234) 时丢弃尾组。
 *
 * 谁调用：**只有 mp_ui_render()**（在末尾按 st->screen 调 mp_hints_for +
 * mp_hints_draw）。固件与 sim 都不要自己叠加底栏。
 */
#include <stdint.h>

#include "mp_ui.h"   /* mp_screen_t */

typedef enum {
    MP_HINT_PREV = 0,   /* ◀ 左键（GPIO2） */
    MP_HINT_NEXT,       /* ▶ 右键（GPIO6） */
    MP_HINT_OK,         /* ○ 确认（GPIO8） */
    MP_HINT_MENU,       /* × 取消/返回（GPIO7） */
    MP_HINT_FUNC,       /* ▭ 胶囊菜单（GPIO4） */
    MP_HINT_KEY_COUNT
} mp_hint_key_t;

#define MP_HINT_CANCEL MP_HINT_MENU     /* × 取消/返回 */
#define MP_HINT_CAPSULE MP_HINT_FUNC    /* ▭ 胶囊菜单键 */

#define MP_HINT_MAX 4
#define MP_KEYCAP_W 14
#define MP_KEYCAP_H 14
#define MP_HINT_X 30            /* 兼容旧宏：首组参考起点 */
#define MP_HINT_X2 144          /* 兼容旧宏：两组第二组参考起点 */
#define MP_HINT_RIGHT 242       /* 内容区右界：250 - 8 px 安全边距 */
#define MP_HINT_GAP 14          /* 组间距（三组屏） */
#define MP_KEYCAP_GAP 2         /* 同一组内元素间距（中文/键帽之间） */
#define MP_HINT_TEXT_TOP 105
#define MP_KEYCAP_TOP 106
#define MP_HINT_MAX_W MP_CONTENT_W   /* 234：底栏内容宽（8..241） */

typedef struct {
    uint8_t nkeys;      /* 0..2 */
    uint8_t key[2];     /* mp_hint_key_t */
    const char *pre;    /* 键帽前的中文（UTF-8），可为 NULL */
    const char *post;   /* 键帽后的中文（UTF-8），可为 NULL */
} mp_hint_item_t;

typedef struct {
    uint8_t n;
    mp_hint_item_t item[MP_HINT_MAX];
} mp_hint_set_t;

/* 1bpp 键帽位图：MP_HINT_KEY_COUNT 组，每组 MP_KEYCAP_H 行 × 2 B，
 * MSB 最左，bit=1 为墨。数值冻结在 PORT_SPEC §1.7.1（mp_hints.c 定义）。 */
extern const uint8_t mp_keycap_bits[MP_HINT_KEY_COUNT][MP_KEYCAP_H * 2];

/* 按屏给出底栏内容（冻结表，PORT_SPEC §2.5）：
 *   CLOCK    : [◀]设置  [▶]书库
 *   SELECT   : [◀][▶]选择  [○]打开  [×]返回
 *   READ     : 空（底栏位置被 2 px 进度条占用）
 *   SETTINGS : [◀][▶]选择  [○]确定  [×]返回
 *   ABOUT    : [×]返回
 *   BOOK_INFO: [◀][▶]选择  [○]确定  [×]返回
 *   JUMP     : [◀][▶]调整  [○]跳转  [×]取消
 *   TIME_SET : [◀][▶]调整  [○]下一步  [×]取消
 *   DELETE   : 长按[○]删除  [×]取消
 *   LOCK     : 空（锁图不叠字；兜底页的"长按[○]解锁"由 mp_ui_lock 自己画）
 *   EMPTY    : [○]刷新  [×]返回
 *   ERROR    : [○]重试  [×]返回
 *   BACKLIGHT: [◀][▶]调整  [○]确定  [×]取消
 *   RESTORE  : 长按[○]恢复  [×]取消
 *   NO_LOCK_IMG: [×]返回
 *   LOW_BATT : 空（整屏提示）
 * 返回值 = 组数（0..MP_HINT_MAX）。 */
uint8_t mp_hints_for(mp_screen_t screen, mp_hint_set_t *out);

/* 实际横向跨度（首组左端到末组右端）；总宽超过 MP_HINT_MAX_W(=234) 即丢弃尾组。 */
int mp_hints_width(const mp_hint_set_t *s);

/* 画到 y=MP_HINT_TOP..121：全部组按实际总宽在 250 px 内水平居中对称排版。
 * s 为 NULL 或 n==0 时什么都不画。 */
void mp_hints_draw(uint8_t *fb, const mp_hint_set_t *s);

/* 单个键帽，(x, ytop) 是 14x14 方框左上角。 */
void mp_hints_keycap(uint8_t *fb, mp_hint_key_t key, int x, int ytop);
