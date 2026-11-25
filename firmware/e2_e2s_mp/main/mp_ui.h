#pragma once
/* mp_ui.h — 250x122 1bpp 界面渲染契约（纯 C，无 ESP-IDF / 无 FreeRTOS）。
 *
 * 这一层是**纯渲染**：只吃 mp_ui_state_t（视图模型）和一块 250x122 的 1bpp
 * 帧缓冲，不读 SD、不读时钟、不碰按键。mp_app.c 负责把状态填进视图模型；
 * sim/ 主机模拟器直接构造视图模型出 PNG。
 *
 * 坐标系：全部是**可见坐标** y=0..121（E2S 逻辑帧，行 32 B，bit1=白）。
 * Miaopaper 的坐标（250x128 画布、顶部 6 行不可见）要用 mp_font.h 的
 * MP_Y(y)=y-6 换过来；本文件里的常量已经是可见坐标，**不要**再减 6。
 *
 * 2026-11-25 UI v3（统一、简洁、对称）：
 *   - 状态栏：左标题 / 居中时间(或向导步数) / 蓝牙开显示符文(关则隐藏) / 描边黑芯电池 + y=18 分隔线；
 *   - 主页只有 Inter 大号时钟与居中底栏；
 *   - 书库与设置统一使用微圆角整行反色高亮条 + 右侧极简滚动条；
 *   - 设置存储行内联容量条，胶囊开关严格 1px 圆角闭合；
 *   - 书籍信息页采用元信息 + 点线 + 2x2 圆角按钮网格；
 *   - 时间设置展示完整 YYYY-MM-DD / HH:MM 并用反色块高亮当前编辑段；
 *   - 跳转页采用 26 px 中号数值 + 带刻度的 200 px 圆形滑块轨道；
 *   - 锁屏支持阅读数据统计页（上下点线 + 双栏统计 + 迷你锁头 + 长按[○]解锁）与纯锁屏图；
 *   - 提示/确认/完成页使用统一体量的 1px 图标与垂直居中排版；
 *   - 底栏全部组按实际总宽水平居中对称排版，返回/取消键帽采用 ×。
 */
#include <stdbool.h>
#include <stdint.h>

#include "mp_page.h"

/* ------------------------------------------------------------------ *
 * 帧
 * ------------------------------------------------------------------ */
#define MP_FB_W 250
#define MP_FB_H 122
#define MP_FB_ROW_BYTES 32
#define MP_FB_BYTES 3904     /* 32 * 122；与 reader_cfg.h READER_FB_BYTES 相同 */

/* ------------------------------------------------------------------ *
 * 栅格（可见坐标）
 * ------------------------------------------------------------------ *
 * y=0..17    状态栏（标题 / 居中时间 / 蓝牙 / 电量），y=18 细分隔线（CLOCK 除外）
 * y=19..103  内容区
 * y=104..121 底栏（键帽 + 中文动词；READ 的底栏位置被 2 px 进度条占用）
 */
#define MP_STATUS_H 18
#define MP_RULE_Y 18                  /* 状态栏底部分隔线；y=19 留白 */
#define MP_STATUS_X 8                 /* 与内容左边距对齐（8 px 安全边距） */
#define MP_STATUS_TEXT_TOP 1          /* 16 px 字形的顶行 */
#define MP_STATUS_TITLE_MAX_W 96      /* x=8..103，给居中时间让位 */
#define MP_STATUS_MID_X 146           /* 兼容旧宏 */
#define MP_STATUS_MID_W 38
#define MP_STATUS_TIME_TOP 1
#define MP_BT_X 194                   /* 蓝牙符文 12x13 */
#define MP_BT_Y 2
#define MP_BATT_X 214                 /* 外壳 26x13 + 正极帽 2x5，右界 x=242 对称左 8 px */
#define MP_BATT_Y 2
#define MP_BATT_W 26
#define MP_BATT_H 13
#define MP_BATT_CAP_W 2
#define MP_BATT_CAP_H 5

#define MP_CONTENT_X 8
#define MP_CONTENT_W 234              /* 8 + 234 + 8 = 250 */

#define MP_LIST_TOP 20                /* 第 0 行顶；4 行块在 19..103 内居中 */
#define MP_LIST_STEP 21               /* 20 px 高亮条 + 1 px 间距 */
#define MP_LIST_ROWS 4                /* 可见 4 行 */
#define MP_SEL_RULE_W 16              /* 兼容旧函数原型保留 */
#define MP_SEL_RULE_H 1
#define MP_SEL_RULE_DY 18
#define MP_SEL_BAR_X 6                /* 微圆角反色高亮条（书库/设置共用） */
#define MP_SEL_BAR_W 238
#define MP_SEL_BAR_DY 0
#define MP_SEL_BAR_H 20

#define MP_READ_X 2                   /* Miaopaper 正文左右边距 2 */
#define MP_READ_W 246
#define MP_READ_TOP 22                /* 第 0 行顶 = 基线 34；5 行底到 109 */
#define MP_READ_STEP MP_LINE_H        /* 18 */
#define MP_PROGRESS_X 2               /* 2 px 进度条：y=118..119 */
#define MP_PROGRESS_Y 118
#define MP_PROGRESS_H 2
#define MP_PROGRESS_W 246

#define MP_HINT_TOP 104               /* 底栏 18 px 的顶；内容见 mp_hints.h */

/* CLOCK：Inter 66 px，墨迹约 50 px 高，在 18..103 里居中。 */
#define MP_CLOCK_BASE 85              /* ink 36..85 */
#define MP_CLOCK_FIRST 0x25           /* '%'；时钟串本身只用到 0..9 和 ':' */
#define MP_CLOCK_LAST 0x3A

#define MP_MSG_TOP2 44                /* 两行消息默认起点 */
#define MP_MSG_TOP3 34                /* 三行消息默认起点 */
#define MP_MSG_STEP 20

/* BOOK_INFO：元信息 + 点线 + 2x2 圆角按钮网格。 */
#define MP_INFO_META_TOP 22
#define MP_INFO_RULE_Y 41
#define MP_INFO_BTN_W 114
#define MP_INFO_BTN_H 22
#define MP_INFO_BTN_GAP_X 6
#define MP_INFO_BTN_GAP_Y 6
#define MP_INFO_BTN_TOP 47
#define MP_INFO_ACT_TOP 37
#define MP_INFO_ACT_STEP 17
#define MP_INFO_ACT_ROWS 4

/* JUMP / TIME_SET：布局常量。 */
#define MP_BIG_VALUE_BASE 74          /* 兼容旧宏保留 */
#define MP_TIME_LABEL_TOP 82          /* 兼容旧宏保留 */
#define MP_TIME_FIELDS 5

/* TIME_SET：双行完整日期时间（YYYY-MM-DD / HH:MM）+ 当前字段反色高亮。 */
#define MP_TIME_BOX_H 27              /* 19 px 数字 + 上下各 4 px 边距 */
#define MP_TIME_PAD_X 4               /* 高亮块左右额外内边距 */
#define MP_TIME_DATE_Y 29             /* 日期行高亮块顶（29..55，基线 52） */
#define MP_TIME_CLOCK_Y 66            /* 时间行高亮块顶（66..92，基线 89） */
#define MP_TIME_SEP_W 16              /* '-' 与 ':' 分隔槽宽 */
#define MP_TIME_YEAR_W 80             /* 4*18 + 2*4 */
#define MP_TIME_TWO_W 44              /* 2*18 + 2*4 */
#define MP_TIME_DATE_X 25             /* (250 - (80+16+44+16+44)) / 2 */
#define MP_TIME_CLOCK_X 73            /* (250 - (44+16+44)) / 2 */

/* JUMP：中号百分比读数 + 200 px 精致进度滑块轨道 + 刻度标注。 */
#define MP_JUMP_VAL_BASE 52           /* 数值基线（33..52） */
#define MP_JUMP_TRACK_X 25            /* 轨道左端（左右各留 25 px） */
#define MP_JUMP_TRACK_W 200           /* 轨道总宽（x=25..224） */
#define MP_JUMP_TRACK_Y 71            /* 轨道中心线 */
#define MP_JUMP_LABEL_Y 82            /* 5x7 刻度标注顶（82..88） */

/* 锁屏兜底页（无锁图时）。 */
#define MP_LOCK_ICON_X 115
#define MP_LOCK_ICON_Y 28
#define MP_LOCK_TEXT_TOP 58
#define MP_LOCK_HINT_X 84
#define MP_LOCK_HINT_TOP 105

/* ------------------------------------------------------------------ *
 * 视图模型
 * ------------------------------------------------------------------ */
typedef enum {
    MP_SCREEN_CLOCK = 0,
    MP_SCREEN_SELECT,     /* 书库列表 */
    MP_SCREEN_READ,
    MP_SCREEN_SETTINGS,
    MP_SCREEN_ABOUT,
    MP_SCREEN_LOCK,
    MP_SCREEN_EMPTY,
    MP_SCREEN_ERROR,
    MP_SCREEN_RESTORE,
    MP_SCREEN_BOOK_INFO,  /* READ 的 MENU 信息页 */
    MP_SCREEN_JUMP,       /* 跳转到百分比 */
    MP_SCREEN_DELETE,     /* 删除本书确认页 */
    MP_SCREEN_TIME_SET,   /* 年/月/日/时/分 向导 */
    MP_SCREEN_TIME_DONE,  /* "设置完成" 过渡页 */
    MP_SCREEN_NO_LOCK_IMG,/* 未上传锁屏图提示页 */
    MP_SCREEN_BACKLIGHT,  /* 背光设置页（4 档滑条 + 快捷手势说明） */
    MP_SCREEN_LOW_BATT,   /* 电池空自动关机提示页（无状态栏/底栏） */
    MP_SCREEN_COUNT
} mp_screen_t;

/* 三种文字来源（缺一不可，别压成两值）：
 *   MP_STR_UTF8   —— 界面固定文案（"返回"/"设置"/"已锁定"…）；text_draw_line()
 *   MP_STR_ASCII  —— 英文书名等 ASCII 字节；mp_text_draw(enc=MP_ENC_ASCII)
 *   MP_STR_GB2312 —— 中文书名等 GB2312 字节；mp_text_draw(enc=MP_ENC_GB2312)
 * 注意 MP_STR_UTF8==0 与 MP_ENC_ASCII==0 数值相同但语义不同：不要混用。 */
typedef enum {
    MP_STR_UTF8 = 0,
    MP_STR_ASCII = 1,
    MP_STR_GB2312 = 2
} mp_str_enc_t;

typedef struct {
    const char *text;   /* NUL 结尾；NULL 表示不画 */
    uint8_t enc;        /* mp_str_enc_t */
} mp_str_t;

/* 设置行的右侧控件。书库行只用 MP_ROW_TEXT。 */
typedef enum {
    MP_ROW_TEXT = 0,     /* 纯文字行（书库） */
    MP_ROW_TOGGLE,       /* 开关，右侧 26x12；on=1 */
    MP_ROW_VALUE,        /* label 左、value 右 */
    MP_ROW_STORAGE,      /* label + value + 2 px 容量条；不可选中 */
    MP_ROW_ACTION        /* label 左、右侧 chevron */
} mp_ui_row_kind_t;

typedef struct {
    const char *label;  /* NUL 结尾；NULL 表示空行 */
    uint8_t enc;        /* mp_str_enc_t */
    const char *value;  /* 右侧值（UTF-8），可为 NULL */
    uint8_t kind;       /* mp_ui_row_kind_t */
    uint8_t on;         /* MP_ROW_TOGGLE：1=开 */
    uint8_t bar;        /* MP_ROW_STORAGE：已用百分比 0..100 */
} mp_ui_row_t;

/* 锁屏页面三种呈现态：阅读数据统计 / 纯锁屏图 / 解锁提示。 */
typedef enum {
    MP_LOCK_MODE_STATS = 0,   /* 锁屏阅读数据统计（自带锁头 + 长按[○]解锁） */
    MP_LOCK_MODE_IMAGE = 1,   /* 纯锁屏图（4000 B 逐像素映射，不叠文字） */
    MP_LOCK_MODE_PROMPT = 2,  /* 锁屏图按其他键后跳转的解锁提示页 */
} mp_lock_mode_t;

typedef struct {
    /* --- 通用 --- */
    uint8_t screen;             /* mp_screen_t */

    /* 状态栏：左标题 / 中段 / 居中时间 / 蓝牙 / 电量 */
    mp_str_t left;
    const char *mid;            /* UTF-8；NULL 或空串 = 不画 */
    uint8_t hour, min;          /* 0..23 / 0..59 */
    uint8_t time_valid;         /* 0 = 不画状态栏时间（CLOCK/TIME_SET/JUMP） */
    uint8_t bt_on;              /* 1 = 显示蓝牙图标；0 = 不显示 */
    uint8_t battery;            /* 0..100 */
    uint8_t battery_valid;      /* 0 = 只画外壳 */
    uint8_t charging;           /* 1 = 外壳左侧 '+' */

    /* --- 日期 / 时间设置 --- */
    uint16_t year;
    uint8_t month, day, week;   /* week: 1=周一 … 7=周日 */
    uint8_t time_step;          /* 0=年 1=月 2=日 3=时 4=分 */

    /* --- select / settings / info 列表 --- */
    uint8_t row_count;          /* 逻辑行数（可 > MP_LIST_ROWS） */
    uint8_t sel;                /* 选中下标（逻辑行号） */
    uint8_t scroll;             /* 首行下标 */
    mp_ui_row_t row[MP_LIST_ROWS];

    /* --- read --- */
    uint8_t line_count;
    uint8_t line_enc;           /* MP_ENC_* */
    char line[MP_READ_LINES][MP_LINE_BYTES];
    uint8_t percent;            /* 0..100；只驱动 2 px 进度条 */

    /* --- about --- */
    uint8_t about_lines;
    mp_str_t about_line[MP_LIST_ROWS];

    /* --- book info --- */
    mp_str_t meta;              /* "字数 约12.3万  进度 42%  大小 1.2MB" */

    /* --- jump --- */
    uint8_t jump_pct;           /* 0..100 */

    /* --- lock --- */
    const uint8_t *lock_img;
    uint8_t lock_valid;
    uint8_t lock_mode;          /* mp_lock_mode_t */
    uint32_t read_mins;         /* 累计阅读分钟数 */
    uint32_t read_chars;        /* 累计阅读字数 */

    /* --- empty / error / restore / delete / time done --- */
    uint8_t msg_lines;
    mp_str_t msg_line[3];

    /* --- backlight --- */
    uint8_t bl_level;           /* 背光亮度 0..3（0=关闭） */
    uint8_t bl_popup;           /* 1 = 居中叠加快捷背光调整浮窗 */

    /* 1 = 不画底栏（恢复成功页 / 时间设置完成页）。 */
    uint8_t hide_hints;
} mp_ui_state_t;

/* 列表滚动（冻结规则）：装得下就不滚；装不下时让选中项尽量贴底。 */
static inline uint8_t mp_ui_list_scroll(uint8_t sel, uint8_t row_count)
{
    if (row_count <= MP_LIST_ROWS) {
        return 0;
    }
    int s = (int)sel - (MP_LIST_ROWS - 1);
    if (s < 0) {
        s = 0;
    }
    int max = (int)row_count - MP_LIST_ROWS;
    if (s > max) {
        s = max;
    }
    return (uint8_t)s;
}

/* ------------------------------------------------------------------ *
 * 渲染入口
 * ------------------------------------------------------------------ */
void mp_ui_render(const mp_ui_state_t *st, uint8_t *fb);
void mp_ui_clear(uint8_t *fb);

/* 分件渲染（mp_ui_render 内部按屏调用；都不画底栏）。 */
void mp_ui_status_bar(uint8_t *fb, const mp_ui_state_t *st);
void mp_ui_battery(uint8_t *fb, int x, int y, uint8_t pct, uint8_t valid,
                   uint8_t charging);
void mp_ui_clock(uint8_t *fb, const mp_ui_state_t *st);
void mp_ui_list(uint8_t *fb, const mp_ui_state_t *st);
void mp_ui_settings(uint8_t *fb, const mp_ui_state_t *st);
void mp_ui_about(uint8_t *fb, const mp_ui_state_t *st);
void mp_ui_read(uint8_t *fb, const mp_ui_state_t *st);
void mp_ui_progress(uint8_t *fb, uint8_t pct);
void mp_ui_msg(uint8_t *fb, const mp_ui_state_t *st);
void mp_ui_lock(uint8_t *fb, const mp_ui_state_t *st);
void mp_ui_book_info(uint8_t *fb, const mp_ui_state_t *st);
void mp_ui_jump(uint8_t *fb, const mp_ui_state_t *st);
void mp_ui_time_set(uint8_t *fb, const mp_ui_state_t *st);
void mp_ui_backlight(uint8_t *fb, const mp_ui_state_t *st);
void mp_ui_backlight_popup(uint8_t *fb, const mp_ui_state_t *st);
void mp_ui_low_batt(uint8_t *fb);

/* ------------------------------------------------------------------ *
 * 文本工具
 * ------------------------------------------------------------------ */
/* UI 文字：ASCII 用 Inter_UI_16，中文走 HZK16；两者基线对齐。
 * 返回/接受的坐标都是 16 px 行的顶行 top。 */
int mp_ui_text_w(const char *utf8);
void mp_ui_text(uint8_t *fb, int x, int top, const char *utf8, int max_w);
void mp_ui_text_center(uint8_t *fb, int top, const char *utf8);
/* black=0 用于反色行（白字黑底）。 */
void mp_ui_text_color(uint8_t *fb, int x, int top, const char *utf8, int max_w,
                      int black);

/* GB2312 / ASCII 原始字节一行，左上角 (x, top)。
 * GB2312 走 HZK16；ASCII 走 Dialog_plain_16（阅读正文的字形），
 * 基线分别取 top+12 / top+15。 */
void mp_ui_gb_text(uint8_t *fb, int x, int top, const char *s, uint8_t enc,
                   int max_w);
void mp_ui_gb_text_color(uint8_t *fb, int x, int top, const char *s, uint8_t enc,
                         int max_w, int black);

/* 书库选中底线（固定 16x1，水平居中于文字）。 */
void mp_ui_underline(uint8_t *fb, int row_top, int text_w);
