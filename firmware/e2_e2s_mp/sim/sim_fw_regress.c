/* sim_fw_regress.c — 本机全链路固件回归测试套件。
 *
 * 将真实固件核心源码（mp_app.c / mp_store.c / mp_toc.c / mp_ota.c /
 * recovery.c / mp_ui.c / mp_hints.c / mp_page.c / mp_font.c / gfx.c / text）
 * 链接至本机虚拟硬件层（sim_hal.c），在进程级隔离（fork）与独立虚拟 SD 沙盒下，
 * 端到端回归测试启动、充电弹窗、背光与自动锁屏、BLE 书库与锁屏图传输、
 * 章节目录缓存与跳章、跨深睡走时与误触过滤、低电保护、原地 BLE OTA（支持加载真实
 * build/miaopaper_e2_e2s.bin）及恢复原厂固件等全链路行为。
 */
#include <dirent.h>
#include <errno.h>
#include <setjmp.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "board.h"
#include "bootloader_common.h"
#include "display.h"
#include "esp_app_format.h"
#include "esp_flash_partitions.h"
#include "esp_image_format.h"
#include "esp_ota_ops.h"
#include "esp_sleep.h"
#include "input.h"
#include "mp_app.h"
#include "mp_ble.h"
#include "mp_ota.h"
#include "mp_store.h"
#include "mp_toc.h"
#include "mp_ui.h"
#include "power_filter.h"
#include "rc_common.h"
#include "recovery.h"
#include "sdkconfig.h"
#include "sim_hal.h"
#include "sim_text.h"

/* ------------------------------------------------------------------ *
 * 断言与沙盒管理工具
 * ------------------------------------------------------------------ */
static void fail_at(const char *file, int line, const char *expr)
{
    fprintf(stderr, "  [断言失败] %s:%d: %s\n", file, line, expr);
    _exit(1);
}

#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            fail_at(__FILE__, __LINE__, #cond);                                \
        }                                                                      \
    } while (0)

#define CHECK_EQ_U(got, want)                                                  \
    do {                                                                       \
        unsigned long long _g = (unsigned long long)(got);                     \
        unsigned long long _w = (unsigned long long)(want);                    \
        if (_g != _w) {                                                        \
            fprintf(stderr, "  [断言失败] %s:%d: %s == %llu (期望 %llu)\n",     \
                    __FILE__, __LINE__, #got, _g, _w);                         \
            _exit(1);                                                          \
        }                                                                      \
    } while (0)

static void rm_rf_dir(const char *dir)
{
    DIR *d = opendir(dir);
    if (!d) {
        return;
    }
    struct dirent *ent;
    char path[1024];
    while ((ent = readdir(d)) != NULL) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) {
            continue;
        }
        int n = snprintf(path, sizeof(path), "%s/%s", dir, ent->d_name);
        if (n <= 0 || (size_t)n >= sizeof(path)) {
            continue;
        }
        struct stat st;
        if (lstat(path, &st) == 0 && S_ISDIR(st.st_mode)) {
            rm_rf_dir(path);
        } else {
            (void)unlink(path);
        }
    }
    closedir(d);
    (void)rmdir(dir);
}

/* ------------------------------------------------------------------ *
 * 固件交互辅助函数（按键 / 时间推进 / BLE / 像素检查）
 * ------------------------------------------------------------------ */
static uint32_t now32(void)
{
    return (uint32_t)sim_hal_now_ms();
}

static void tick_now(void)
{
    mp_app_tick(now32());
}

static void advance_ticks(uint64_t total_ms, uint64_t step_ms)
{
    uint64_t done = 0;
    while (done < total_ms) {
        uint64_t step = total_ms - done;
        if (step > step_ms) {
            step = step_ms;
        }
        sim_hal_advance_ms(step);
        tick_now();
        done += step;
    }
}

static void press_click(key_id_t key)
{
    mp_app_key((uint8_t)key, KEY_EV_DOWN, now32());
    sim_hal_advance_ms(20);
    mp_app_key((uint8_t)key, KEY_EV_CLICK, now32());
    tick_now();
}

static void press_ok_single(void)
{
    mp_app_key(KEY_OK, KEY_EV_DOWN, now32());
    sim_hal_advance_ms(20);
    mp_app_key(KEY_OK, KEY_EV_CLICK, now32());
    sim_hal_advance_ms(MP_DCLICK_MS + 10u);
    tick_now();
}

static void press_ok_double(void)
{
    mp_app_key(KEY_OK, KEY_EV_DOWN, now32());
    sim_hal_advance_ms(20);
    mp_app_key(KEY_OK, KEY_EV_CLICK, now32());
    sim_hal_advance_ms(100);
    mp_app_key(KEY_OK, KEY_EV_DOWN, now32());
    sim_hal_advance_ms(20);
    mp_app_key(KEY_OK, KEY_EV_CLICK, now32());
    tick_now();
}

static void press_long(key_id_t key)
{
    mp_app_key((uint8_t)key, KEY_EV_DOWN, now32());
    sim_hal_advance_ms(MP_LONG_MS);
    mp_app_key((uint8_t)key, KEY_EV_LONG, now32());
    tick_now();
}

/* 模拟锁屏态按住 ○ 键持续 hold_ms 毫秒：满 2 秒时在尚未松手前即应立即唤醒，
 * 未满 2 秒提前松手则不解锁。 */
static void hold_lock_ok(uint32_t hold_ms)
{
    sim_hal_set_gpio(BOARD_KEY_OK_GPIO, 0);
    mp_app_key(KEY_OK, KEY_EV_DOWN, now32());
    uint32_t done = 0;
    bool long_sent = false;
    while (done < hold_ms) {
        uint32_t step = hold_ms - done;
        if (step > 50u) {
            step = 50u;
        }
        sim_hal_advance_ms(step);
        done += step;
        if (!long_sent && done >= MP_LONG_MS) {
            long_sent = true;
            mp_app_key(KEY_OK, KEY_EV_LONG, now32());
        }
        tick_now();
    }
    /* 若 hold_ms >= MP_LOCK_UNLOCK_MS，在松手前就已经应该解锁完毕！ */
    if (hold_ms >= MP_LOCK_UNLOCK_MS) {
        CHECK(mp_app_state.screen != MP_SCREEN_LOCK);
    }
    sim_hal_set_gpio(BOARD_KEY_OK_GPIO, 1);
    if (!long_sent) {
        mp_app_key(KEY_OK, KEY_EV_CLICK, now32());
    }
    sim_hal_advance_ms(50u);
    tick_now();
}

static void ble_send(uint8_t op, const void *payload, uint8_t len)
{
    mp_ble_req_t req;
    memset(&req, 0, sizeof(req));
    req.op = op;
    req.len = len;
    if (payload && len > 0) {
        memcpy(req.data, payload, len);
    }
    mp_app_ble_req(&req);
}

static int fb_px_black(const uint8_t *fb, int x, int y)
{
    uint8_t mask = (uint8_t)(0x80u >> (x & 7));
    return (fb[y * MP_FB_ROW_BYTES + (x >> 3)] & mask) == 0;
}

static int fb_rect_ink(const uint8_t *fb, int x0, int y0, int x1, int y1)
{
    int ink = 0;
    for (int y = y0; y <= y1; y++) {
        for (int x = x0; x <= x1; x++) {
            if (fb_px_black(fb, x, y)) {
                ink++;
            }
        }
    }
    return ink;
}

/* 通过真实 BLE 0x10/0x11/0x12 协议上传一本电子书，返回分配的 16-bit 槽号。 */
static uint16_t ble_upload_book(const char *title, const uint8_t *body,
                                uint32_t body_len, uint8_t enc)
{
    size_t tlen = strlen(title);
    CHECK(tlen >= 2 && tlen <= MP_BLE_TITLE_MAX);

    uint8_t begin_buf[32];
    begin_buf[0] = (uint8_t)tlen;
    memcpy(begin_buf + 1, title, tlen);
    begin_buf[1 + tlen] = (uint8_t)((body_len >> 16) & 0xFFu);
    begin_buf[2 + tlen] = (uint8_t)((body_len >> 8) & 0xFFu);
    begin_buf[3 + tlen] = (uint8_t)(body_len & 0xFFu);
    begin_buf[4 + tlen] = enc;

    sim_hal_ble_clear_tx();
    ble_send(MP_OP_BOOK_BEGIN, begin_buf, (uint8_t)(tlen + 5u));
    CHECK(sim_hal_ble_tx_count() == 1u);
    const sim_ble_pkt_t *rsp = sim_hal_ble_tx_at(0);
    CHECK_EQ_U(rsp->len, 4u);
    CHECK_EQ_U(rsp->data[0], MP_OP_BOOK_BEGIN);
    CHECK_EQ_U(rsp->data[1], MP_ST_OK);
    uint16_t slot = (uint16_t)rsp->data[2] | ((uint16_t)rsp->data[3] << 8);
    CHECK(slot < MP_MAX_SLOTS);
    CHECK_EQ_U(mp_app_state.uploading, 1u);
    CHECK_EQ_U(mp_ble_upload_active(), 1u);

    uint32_t off = 0;
    while (off < body_len) {
        uint32_t chunk = body_len - off;
        if (chunk > 200u) {
            chunk = 200u;
        }
        ble_send(MP_OP_BOOK_DATA, body + off, (uint8_t)chunk);
        off += chunk;
    }

    sim_hal_ble_clear_tx();
    ble_send(MP_OP_BOOK_END, NULL, 0);
    CHECK(sim_hal_ble_tx_count() == 1u);
    rsp = sim_hal_ble_tx_at(0);
    CHECK_EQ_U(rsp->len, 7u);
    CHECK_EQ_U(rsp->data[0], MP_OP_BOOK_END);
    CHECK_EQ_U(rsp->data[1], MP_ST_OK);
    uint16_t end_slot = (uint16_t)rsp->data[2] | ((uint16_t)rsp->data[6] << 8);
    CHECK_EQ_U(end_slot, slot);
    uint32_t written = ((uint32_t)rsp->data[3] << 16) |
                       ((uint32_t)rsp->data[4] << 8) | rsp->data[5];
    CHECK_EQ_U(written, body_len);
    CHECK_EQ_U(mp_app_state.uploading, 0u);
    CHECK_EQ_U(mp_ble_upload_active(), 0u);
    return slot;
}

/* 准备带特征指纹的 8 MiB Flash（含原厂槽 paperang_e2 与第三方槽 miaopaper_e2_e2s）。 */
static void setup_dual_slot_flash(unsigned running_slot, uint32_t ota_seq_s0,
                                  uint32_t ota_seq_s1)
{
    uint8_t *fl = sim_hal_flash_raw();
    memset(fl, 0xFF, SIM_FLASH_SIZE);

    /* 在禁写区打特征指纹，事后用于零修改逐字节校验。 */
    for (uint32_t a = 0; a < 0x8000u; a++) {
        fl[a] = (uint8_t)((a * 17u + 3u) & 0xFFu); /* Bootloader */
    }
    for (uint32_t a = 0x8000u; a < 0x9000u; a++) {
        fl[a] = (uint8_t)((a * 29u + 7u) & 0xFFu); /* 分区表 */
    }
    for (uint32_t a = SIM_PART_NVS_ADDR; a < SIM_PART_NVS_ADDR + 0x6000u; a++) {
        fl[a] = (uint8_t)((a * 31u + 11u) & 0xFFu); /* NVS */
    }
    for (uint32_t a = SIM_PART_PHY_ADDR; a < SIM_PART_PHY_ADDR + 0x1000u; a++) {
        fl[a] = (uint8_t)((a * 13u + 5u) & 0xFFu); /* phy_init */
    }
    for (uint32_t a = SIM_STOCK_FONT_ADDR;
         a < SIM_STOCK_FONT_ADDR + SIM_STOCK_FONT_SIZE; a++) {
        fl[a] = (uint8_t)((a * 43u + 19u) & 0xFFu); /* 原厂字体区 */
    }

    /* 另一槽放入合法的原厂 paperang_e2 镜像；当前运行槽放第三方旧镜像。 */
    uint32_t stock_addr = (running_slot == 1u) ? SIM_PART_OTA0_ADDR : SIM_PART_OTA1_ADDR;
    uint32_t third_addr = (running_slot == 1u) ? SIM_PART_OTA1_ADDR : SIM_PART_OTA0_ADDR;

    size_t stock_len = sim_build_esp_image(fl + stock_addr, SIM_PART_OTA_SIZE,
                                           CONFIG_IDF_FIRMWARE_CHIP_ID,
                                           "paperang_e2", "1.3.739",
                                           70 * 1024u, true);
    CHECK(stock_len >= MP_OTA_MIN_IMAGE_SIZE);

    size_t third_len = sim_build_esp_image(fl + third_addr, SIM_PART_OTA_SIZE,
                                           CONFIG_IDF_FIRMWARE_CHIP_ID,
                                           "miaopaper_e2_e2s", "1.1.0",
                                           72 * 1024u, true);
    CHECK(third_len >= MP_OTA_MIN_IMAGE_SIZE);

    sim_hal_flash_set_otadata(0, ota_seq_s0, ESP_OTA_IMG_UNDEFINED);
    sim_hal_flash_set_otadata(1, ota_seq_s1, ESP_OTA_IMG_UNDEFINED);
    sim_hal_flash_set_running_slot(running_slot);
    sim_hal_flash_set_app_desc("miaopaper_e2_e2s", "1.2.0", "Nov 27 2026", "04:00:00");
}

/* ------------------------------------------------------------------ *
 * T01：启动初始化、SHA-256 自检、版本号真源同步与 E2/E2S 自适应
 * ------------------------------------------------------------------ */
static void test_t01_boot_about_and_hw_adapt(void)
{
    /* 先校验内置 SHA-256 与 FIPS 180-4 "abc" 标准测试向量完全一致。 */
    static const uint8_t k_abc_sha[32] = {
        0xba, 0x78, 0x16, 0xbf, 0x8f, 0x01, 0xcf, 0xea,
        0x41, 0x41, 0x40, 0xde, 0x5d, 0xae, 0x22, 0x23,
        0xb0, 0x03, 0x61, 0xa3, 0x96, 0x17, 0x7a, 0x9c,
        0xb4, 0x10, 0xff, 0x61, 0xf2, 0x00, 0x15, 0xad,
    };
    uint8_t digest[32];
    sim_sha256("abc", 3, digest);
    CHECK(memcmp(digest, k_abc_sha, 32) == 0);

    sim_hal_init();
    sim_hal_set_backlight_hw(true);
    sim_hal_flash_set_app_desc("miaopaper_e2_e2s", "1.2.0", "Nov  7 2026", "08:09:10");

    mp_app_boot();
    CHECK_EQ_U(mp_app_state.screen, MP_SCREEN_CLOCK);
    CHECK_EQ_U(mp_app_state.storage_ok, 1u);
    CHECK_EQ_U(mp_app_state.has_backlight, 1u);
    CHECK(sim_hal_display_frames() >= 1u);
    CHECK_EQ_U(sim_hal_display_padding_violations(), 0u);

    /* 时钟屏按左键进设置，核验 E2S 具备 7 行（含背光项）。 */
    press_click(KEY_PREV);
    CHECK_EQ_U(mp_app_state.screen, MP_SCREEN_SETTINGS);
    mp_ui_state_t ui;
    CHECK(mp_app_view(&ui));
    CHECK_EQ_U(ui.row_count, 7u);

    /* 移到第 5 行（关于）并确认进入，校验版本号与构建日期格式化为 YYYY-MM-DD。 */
    for (int i = 0; i < 5; i++) {
        press_click(KEY_NEXT);
    }
    press_ok_single();
    CHECK_EQ_U(mp_app_state.screen, MP_SCREEN_ABOUT);
    CHECK(mp_app_view(&ui));
    CHECK_EQ_U(ui.about_lines, 4u);
    CHECK(strcmp(ui.about_line[0].text, "Miaopaper E2S") == 0);
    CHECK(strcmp(ui.about_line[1].text, "固件 1.2.0") == 0);
    CHECK(strcmp(ui.about_line[2].text, "构建 2026-11-07") == 0);
    CHECK(strstr(ui.about_line[3].text, "存储 ") != NULL);
    CHECK_EQ_U(sim_hal_display_padding_violations(), 0u);
    sim_hal_shutdown();
}

static void test_t01b_e2_no_backlight_and_storage_err(void)
{
    /* E2 无背光 + SD 不可用：必须回退安全默认设置（保持蓝牙开启）并进 ERROR 页。 */
    sim_hal_init();
    sim_hal_set_backlight_hw(false);
    sim_hal_set_sd_ok(false);

    mp_app_boot();
    CHECK_EQ_U(mp_app_state.screen, MP_SCREEN_ERROR);
    CHECK_EQ_U(mp_app_state.storage_ok, 0u);
    CHECK_EQ_U(mp_app_state.has_backlight, 0u);
    CHECK_EQ_U(mp_settings.ble_enabled, 1u);

    /* 恢复 SD 在线，按 OK 键触发错误页重试应正常回到 CLOCK 且设置页为 6 行。 */
    sim_hal_set_sd_ok(true);
    press_ok_single();
    CHECK_EQ_U(mp_app_state.storage_ok, 1u);
    CHECK_EQ_U(mp_app_state.screen, MP_SCREEN_CLOCK);

    press_click(KEY_PREV);
    CHECK_EQ_U(mp_app_state.screen, MP_SCREEN_SETTINGS);
    mp_ui_state_t ui;
    CHECK(mp_app_view(&ui));
    CHECK_EQ_U(ui.row_count, 6u);

    /* 无背光机型长按胶囊菜单键（FUNC）绝不能弹出背光浮窗。 */
    press_long(KEY_FUNC);
    CHECK_EQ_U(mp_app_state.bl_popup, 0u);
    CHECK_EQ_U(sim_hal_display_padding_violations(), 0u);
    sim_hal_shutdown();
}

/* ------------------------------------------------------------------ *
 * T02：充电提示弹窗（5 秒自动消失）+ 顶栏小闪电 + 低电插电唤醒
 * ------------------------------------------------------------------ */
static void test_t02_charge_popup_and_icon(void)
{
    sim_hal_init();
    sim_hal_set_battery(76, 3960, true, false);
    mp_app_boot();

    uint32_t f0 = sim_hal_display_frames();
    /* 插入充电器：GPIO5 拉低触发变化，下一次 tick 立即弹出 5 秒充电浮窗。 */
    sim_hal_set_charging(true, true);
    advance_ticks(100, 100);

    CHECK_EQ_U(mp_app_state.charge_popup, 1u);
    CHECK(sim_hal_display_frames() > f0);

    mp_ui_state_t ui;
    CHECK(mp_app_view(&ui));
    CHECK_EQ_U(ui.charging, 1u);
    CHECK_EQ_U(ui.charge_popup, 1u);
    /* 验证状态栏小闪电区域和屏幕中央弹窗区域均有像素绘制。 */
    CHECK(fb_rect_ink(sim_hal_last_fb(), 190, 2, 215, 16) > 5);
    CHECK(fb_rect_ink(sim_hal_last_fb(), 70, 24, 180, 106) > 100);

    /* 经过 4.8 秒仍在显示；跨过 5.0 秒自动消失，但顶栏小闪电保持开启。 */
    advance_ticks(4800, 200);
    CHECK_EQ_U(mp_app_state.charge_popup, 1u);
    uint32_t f_before_hide = sim_hal_display_frames();
    advance_ticks(300, 100);
    CHECK_EQ_U(mp_app_state.charge_popup, 0u);
    CHECK(sim_hal_display_frames() > f_before_hide);
    CHECK(mp_app_view(&ui));
    CHECK_EQ_U(ui.charging, 1u);
    CHECK_EQ_U(ui.charge_popup, 0u);

    /* 拔出再插入，弹窗再次弹出；且弹窗显示期间按方向键应能正常交互并清除弹窗。 */
    sim_hal_set_charging(false, true);
    advance_ticks(100, 100);
    CHECK_EQ_U(mp_app_state.charge_popup, 0u);

    sim_hal_set_charging(true, true);
    advance_ticks(100, 100);
    CHECK_EQ_U(mp_app_state.charge_popup, 1u);
    press_click(KEY_PREV); /* 进设置页 */
    CHECK_EQ_U(mp_app_state.screen, MP_SCREEN_SETTINGS);
    CHECK_EQ_U(mp_app_state.charge_popup, 0u);
    CHECK_EQ_U(sim_hal_display_padding_violations(), 0u);
    sim_hal_shutdown();
}

static void test_t02b_charge_ext1_wakeup_popup(void)
{
    sim_hal_init();
    sim_hal_set_battery(2, 3520, true, true);
    sim_hal_set_wakeup(1u << ESP_SLEEP_WAKEUP_EXT1, 1ULL << BOARD_CHARGE_DET_GPIO);

    mp_app_early_wake_check();
    mp_app_boot();
    /* 低电但正在充电：绝不低电关机，且因插充电器 EXT1 唤醒直接弹充电弹窗。 */
    CHECK_EQ_U(mp_app_state.screen, MP_SCREEN_CLOCK);
    CHECK_EQ_U(mp_app_state.charge_popup, 1u);
    advance_ticks(MP_CHARGE_POPUP_MS + 100u, 200);
    CHECK_EQ_U(mp_app_state.charge_popup, 0u);
    sim_hal_shutdown();
}

/* ------------------------------------------------------------------ *
 * T03：电子书全生命周期（BLE 上传 -> 阅读翻页 -> 目录缓存与选章跳转 ->
 *      进度节流落盘 -> 跳转/重置 -> 空目录提示 -> 删除与压缩）
 * ------------------------------------------------------------------ */
static void test_t03_ebook_toc_read_lifecycle(void)
{
    sim_hal_init();
    mp_app_boot();
    sim_hal_ble_connect(true, 247);

    /* 构造一本 GB2312 中文多章节测试书：含 3 个真实章节 + 1 行带引号伪章节正文。 */
    static const char k_cn_book[] =
        /* 第一回 甄士隐梦幻识通灵\n */
        "\xB5\xDA\xD2\xBB\xBB\xD8 \xD5\xE7\xCA\xBF\xD2\xFE\xC3\xCE\xBB\xC3\xCA\xB6\xCD\xA8\xC1\xE9\n"
        /* 正文填充（足够跨多页） */
        "\xB4\xCB\xBF\xAA\xBE\xED\xB5\xDA\xD2\xBB\xBB\xD8\xD2\xB2\xA1\xA3"
        "\xD7\xF7\xD5\xDF\xD7\xD4\xD4\xC6\xD4\xF8\xC0\xFA\xB9\xFD\xD2\xBB\xB7\xAC\xC3\xCE\xBB\xC3\xA1\xA3"
        "\xC7\xD2\xCB\xB5\xB5\xB1\xC4\xEA\xD5\xE7\xCA\xBF\xD2\xFE\xC3\xCE\xD6\xD0\xBC\xFB\xB5\xBD\xD2\xBB\xC9\xAE\xD2\xBB\xB5\xC0\xA1\xA3"
        "\xCB\xFB\xC8\xD5\xB7\xE7\xD4\xC6\xB1\xE4\xBB\xC3\xCE\xDE\xB3\xA3\xA3\xAC\xCA\xC0\xCA\xC2\xC8\xE7\xC6\xE5\xBE\xD6\xBE\xD6\xD0\xC8\xCB\xA1\xA3"
        "\xB4\xCB\xBF\xAA\xBE\xED\xB5\xDA\xD2\xBB\xBB\xD8\xD2\xB2\xA1\xA3"
        "\xD7\xF7\xD5\xDF\xD7\xD4\xD4\xC6\xD4\xF8\xC0\xFA\xB9\xFD\xD2\xBB\xB7\xAC\xC3\xCE\xBB\xC3\xA1\xA3"
        "\xC7\xD2\xCB\xB5\xB5\xB1\xC4\xEA\xD5\xE7\xCA\xBF\xD2\xFE\xC3\xCE\xD6\xD0\xBC\xFB\xB5\xBD\xD2\xBB\xC9\xAE\xD2\xBB\xB5\xC0\xA1\xA3"
        "\xCB\xFB\xC8\xD5\xB7\xE7\xD4\xC6\xB1\xE4\xBB\xC3\xCE\xDE\xB3\xA3\xA3\xAC\xCA\xC0\xCA\xC2\xC8\xE7\xC6\xE5\xBE\xD6\xBE\xD6\xD0\xC8\xCB\xA1\xA3\n"
        /* 伪章节正文（带中文双引号，应被目录算法过滤）："第一章"说完就走。 */
        "\xA1\xB0\xB5\xDA\xD2\xBB\xD5\xC2\xA1\xB1\xCB\xB5\xCD\xEA\xBE\xCD\xD7\xDF\xA1\xA3\n"
        /* 第二回 贾夫人仙逝扬州城\n */
        "\xB5\xDA\xB6\xFE\xBB\xD8 \xBC\xD6\xB7\xF2\xC8\xCB\xCF\xC9\xCA\xC5\xD1\xEF\xD6\xDD\xB3\xC7\n"
        "\xC7\xB4\xCB\xB5\xBD\xF0\xC1\xEA\xB3\xC7\xC4\xDA\xB7\xE7\xCE\xEF\xD2\xC0\xC8\xBB\xA1\xA3"
        "\xBC\xD6\xD3\xEA\xB4\xE5\xBF\xBC\xD6\xD0\xBD\xF8\xCA\xBF\xC9\xFD\xC8\xCE\xD6\xAA\xB8\xAE\xA1\xA3\n"
        /* 第三章 归来\n */
        "\xB5\xDA\xC8\xFD\xD5\xC2 \xB9\xE9\xC0\xB4\n"
        "\xC7\xE5\xB7\xE7\xD0\xEC\xC0\xB4\xA3\xAC\xCB\xAE\xB2\xA8\xB2\xBB\xD0\xCB\xA1\xA3\n";

    static const char k_plain_book[] =
        "This is a plain ASCII book without any chapter headings at all.\n"
        "Line two keeps talking about coffee and paper ink screens.\n"
        "Line three finishes the short sample document cleanly.\n";

    /* 1) 上传中文书与无章节英文书 */
    uint16_t slot_cn = ble_upload_book("\xBA\xEC\xC2\xA5\xC3\xCE",
                                       (const uint8_t *)k_cn_book,
                                       (uint32_t)(sizeof(k_cn_book) - 1u),
                                       MP_ENC_GB2312);
    uint16_t slot_en = ble_upload_book("Plain Notes",
                                       (const uint8_t *)k_plain_book,
                                       (uint32_t)(sizeof(k_plain_book) - 1u),
                                       MP_ENC_ASCII);
    CHECK_EQ_U(slot_cn, 0u);
    CHECK_EQ_U(slot_en, 1u);

    /* 核验 0x14 扩展 6 B 书库计数与 0x15 三包背靠背元数据 */
    sim_hal_ble_clear_tx();
    ble_send(MP_OP_BOOK_COUNT, NULL, 0);
    CHECK_EQ_U(sim_hal_ble_tx_count(), 1u);
    const sim_ble_pkt_t *cnt_p = sim_hal_ble_tx_at(0);
    CHECK_EQ_U(cnt_p->len, 6u);
    CHECK_EQ_U(cnt_p->data[0], MP_OP_BOOK_COUNT);
    CHECK_EQ_U(cnt_p->data[1], 2u);
    CHECK_EQ_U((uint16_t)(cnt_p->data[2] | (cnt_p->data[3] << 8)), 2u);
    CHECK_EQ_U((uint16_t)(cnt_p->data[4] | (cnt_p->data[5] << 8)), 8u);

    sim_hal_ble_clear_tx();
    uint8_t idx0 = (uint8_t)slot_cn;
    ble_send(MP_OP_BOOK_GET, &idx0, 1);
    CHECK_EQ_U(sim_hal_ble_tx_count(), 3u);
    CHECK_EQ_U(sim_hal_ble_tx_at(0)->data[0], MP_OP_BOOK_GET);

    /* 2) 进入书库 -> 打开第一本（中文书） -> 翻页与 OK 双击回翻 */
    press_click(KEY_NEXT); /* CLOCK -> SELECT */
    CHECK_EQ_U(mp_app_state.screen, MP_SCREEN_SELECT);
    press_ok_single(); /* 打开 slot 0 */
    CHECK_EQ_U(mp_app_state.screen, MP_SCREEN_READ);
    CHECK_EQ_U(mp_app_state.book_idx, slot_cn);
    CHECK_EQ_U(mp_app_state.char_pos, 0u);

    press_click(KEY_NEXT); /* 下一页 */
    uint32_t page2_pos = mp_app_state.char_pos;
    CHECK(page2_pos > 0u);
    CHECK_EQ_U(mp_app_state.pos_dirty, 1u);

    press_click(KEY_NEXT); /* 再下一页 */
    uint32_t page3_pos = mp_app_state.char_pos;
    CHECK(page3_pos > page2_pos);

    /* OK 双击返回上一页 */
    press_ok_double();
    CHECK_EQ_U(mp_app_state.char_pos, page2_pos);

    /* 校验进度落盘节流：未满 2 秒还在脏态，满 2 秒后自动落盘到 progress.bin */
    advance_ticks(1000, 500);
    CHECK_EQ_U(mp_app_state.pos_dirty, 1u);
    advance_ticks(1200, 400);
    CHECK_EQ_U(mp_app_state.pos_dirty, 0u);
    uint32_t disk_pos = 0;
    CHECK(mp_progress_load(slot_cn, &disk_pos));
    CHECK_EQ_U(disk_pos, page2_pos);

    /* 3) 胶囊菜单键打开 BOOK_INFO -> 选第 0 项“目录” */
    press_click(KEY_FUNC);
    CHECK_EQ_U(mp_app_state.screen, MP_SCREEN_BOOK_INFO);
    mp_ui_state_t ui;
    CHECK(mp_app_view(&ui));
    CHECK_EQ_U(ui.row_count, 5u);
    CHECK(strcmp(ui.row[0].label, "目录") == 0);
    CHECK(strcmp(ui.row[1].label, "跳转") == 0);

    press_ok_single(); /* 进入目录页 */
    CHECK_EQ_U(mp_app_state.screen, MP_SCREEN_TOC);
    CHECK_EQ_U(mp_app_state.toc_count, 3u); /* 伪章节必须被过滤，精确识别 3 章 */
    struct stat st_toc;
    CHECK(stat("./mp/toc0.bin", &st_toc) == 0); /* 缓存索引已写入 /sdcard/mp/toc0.bin */

    /* 移动到第 3 章（index=2）并确认跳章 */
    mp_app_state.toc_sel = 0;
    press_click(KEY_NEXT);
    press_click(KEY_NEXT);
    CHECK_EQ_U(mp_app_state.toc_sel, 2u);
    press_ok_single();
    CHECK_EQ_U(mp_app_state.screen, MP_SCREEN_READ);

    /* 校验 char_pos 精确落在“第三章”首字节 */
    const char *ch3 = strstr(k_cn_book, "\xB5\xDA\xC8\xFD\xD5\xC2");
    CHECK(ch3 != NULL);
    CHECK_EQ_U(mp_app_state.char_pos, (uint32_t)(ch3 - k_cn_book));

    /* 4) 再次进入 BOOK_INFO -> 选中“重置进度”(index=2) 原地归零，按返回回 READ */
    press_click(KEY_FUNC);
    press_click(KEY_NEXT);
    press_click(KEY_NEXT);
    press_ok_single();
    CHECK_EQ_U(mp_app_state.screen, MP_SCREEN_BOOK_INFO);
    CHECK_EQ_U(mp_app_state.char_pos, 0u);
    press_click(KEY_MENU); /* BOOK_INFO -> READ */
    CHECK_EQ_U(mp_app_state.screen, MP_SCREEN_READ);

    /* 5) 返回书库打开无章节英文书 -> 打开目录应进入 MP_SCREEN_TOC_EMPTY */
    press_click(KEY_MENU);
    CHECK_EQ_U(mp_app_state.screen, MP_SCREEN_SELECT);
    press_click(KEY_NEXT);
    press_ok_single();
    CHECK_EQ_U(mp_app_state.book_idx, slot_en);
    press_click(KEY_FUNC);
    press_ok_single();
    CHECK_EQ_U(mp_app_state.screen, MP_SCREEN_TOC_EMPTY);
    /* 在空目录页按确认键一键跳转到进度跳转页 */
    press_ok_single();
    CHECK_EQ_U(mp_app_state.screen, MP_SCREEN_JUMP);

    /* 6) 通过 BLE 0x13 删除中文书 -> 校验 toc0.bin 一并被删除，再发 0x1A 压缩目录 */
    sim_hal_ble_clear_tx();
    ble_send(MP_OP_BOOK_DELETE, &idx0, 1);
    CHECK_EQ_U(sim_hal_ble_tx_count(), 1u);
    CHECK_EQ_U(sim_hal_ble_tx_at(0)->data[1], MP_ST_OK);
    CHECK(stat("./mp/toc0.bin", &st_toc) != 0);

    sim_hal_ble_clear_tx();
    ble_send(MP_OP_CATALOG_COMPACT, NULL, 0);
    CHECK_EQ_U(sim_hal_ble_tx_count(), 1u);
    CHECK_EQ_U(sim_hal_ble_tx_at(0)->data[1], MP_ST_OK);
    CHECK_EQ_U(mp_store_count_done(), 1u);
    CHECK_EQ_U(sim_hal_display_padding_violations(), 0u);
    sim_hal_shutdown();
}

/* ------------------------------------------------------------------ *
 * T04：电子书上传异常防御（8 槽满 / 空间不足 / 长度不符 / 断连回收 / 超时）
 * ------------------------------------------------------------------ */
static void test_t04_upload_guards_and_recovery(void)
{
    sim_hal_init();
    mp_app_boot();
    sim_hal_ble_connect(true, 247);

    /* 1) 参数非法（total == 0 / 书名过短） */
    uint8_t bad1[8] = {1, 'A', 0, 0, 10, 0};
    sim_hal_ble_clear_tx();
    ble_send(MP_OP_BOOK_BEGIN, bad1, 6);
    CHECK_EQ_U(sim_hal_ble_tx_at(0)->data[1], MP_ST_BAD_PARAM);

    /* 2) 空间不足拦截 */
    sim_hal_set_fat_space(1024 * 1024, 32 * 1024); /* 剩余 < total + 64 KiB */
    uint8_t ok_hdr[12] = {4, 'T', 'e', 's', 't', 0, 0, 100, 0};
    sim_hal_ble_clear_tx();
    ble_send(MP_OP_BOOK_BEGIN, ok_hdr, 9);
    CHECK_EQ_U(sim_hal_ble_tx_at(0)->data[1], MP_ST_NO_SPACE);
    sim_hal_set_fat_space(64ULL * 1024 * 1024, 48ULL * 1024 * 1024);

    /* 3) 实写长度不足时 0x12 拒绝提交且不污染目录 */
    sim_hal_ble_clear_tx();
    ble_send(MP_OP_BOOK_BEGIN, ok_hdr, 9);
    CHECK_EQ_U(sim_hal_ble_tx_at(0)->data[1], MP_ST_OK);
    uint8_t part[20] = "short payload";
    ble_send(MP_OP_BOOK_DATA, part, 13);
    sim_hal_ble_clear_tx();
    ble_send(MP_OP_BOOK_END, NULL, 0);
    CHECK_EQ_U(sim_hal_ble_tx_at(0)->data[1], MP_ST_FAIL);
    CHECK_EQ_U(mp_store_count_done(), 0u);
    CHECK_EQ_U(mp_app_state.uploading, 0u);

    /* 4) 上传中途断连自动回收会话 */
    ble_send(MP_OP_BOOK_BEGIN, ok_hdr, 9);
    CHECK_EQ_U(mp_app_state.uploading, 1u);
    sim_hal_ble_connect(false, 247);
    advance_ticks(100, 100);
    CHECK_EQ_U(mp_app_state.uploading, 0u);
    CHECK_EQ_U(mp_ble_upload_active(), 0u);
    sim_hal_ble_connect(true, 247);

    /* 5) 突破原 8 本上限：连续上传 20 本书（跨越 8 -> 16 -> 24 动态扩容） */
    static const char sample_long[] =
        "Chapter 1 Dynamic Library Growth Test\n"
        "Page one paragraph with enough characters to wrap across lines on the "
        "250x122 e-ink screen, verifying dynamic catalog storage without an 8-book cap.\n"
        "Page two continues here so that reading position and progress persistence "
        "can be verified accurately even on slots far beyond index seven.\n"
        "Page three confirms random access on high slot numbers.\n";
    const uint32_t sample_len = (uint32_t)(sizeof(sample_long) - 1u);
    for (uint16_t i = 0; i < 20u; i++) {
        char name[20];
        snprintf(name, sizeof(name), "Book_%02u", (unsigned)i);
        uint16_t got_slot = ble_upload_book(name, (const uint8_t *)sample_long,
                                            sample_len, MP_ENC_ASCII);
        CHECK_EQ_U(got_slot, i);
    }
    CHECK_EQ_U(mp_store_count_done(), 20u);
    CHECK_EQ_U(mp_store_slot_capacity(), 24u);

    /* 校验 0x14 返回真实书数 20 与动态槽上界 24 */
    sim_hal_ble_clear_tx();
    ble_send(MP_OP_BOOK_COUNT, NULL, 0);
    CHECK_EQ_U(sim_hal_ble_tx_count(), 1u);
    const sim_ble_pkt_t *cnt_rsp = sim_hal_ble_tx_at(0);
    CHECK_EQ_U(cnt_rsp->len, 6u);
    CHECK_EQ_U(cnt_rsp->data[1], 20u);
    CHECK_EQ_U((uint16_t)(cnt_rsp->data[2] | (cnt_rsp->data[3] << 8)), 20u);
    CHECK_EQ_U((uint16_t)(cnt_rsp->data[4] | (cnt_rsp->data[5] << 8)), 24u);

    /* 校验 0x15 用 2 B 小端槽号读取第 19 本元数据 */
    uint8_t get19[2] = {19u, 0u};
    sim_hal_ble_clear_tx();
    ble_send(MP_OP_BOOK_GET, get19, 2);
    CHECK_EQ_U(sim_hal_ble_tx_count(), 3u);
    CHECK_EQ_U(sim_hal_ble_tx_at(0)->data[1], 19u);
    CHECK(memcmp(sim_hal_ble_tx_at(0)->data + 3, "Book_19", 7) == 0);

    /* 校验书库 UI 顶栏不再显示 "/8" 且可滚动到第 20 本打开阅读并保存进度 */
    press_click(KEY_NEXT); /* CLOCK -> SELECT */
    CHECK_EQ_U(mp_app_state.screen, MP_SCREEN_SELECT);
    mp_ui_state_t ui;
    CHECK(mp_app_view(&ui));
    CHECK(ui.left.text != NULL && strcmp(ui.left.text, "书库 20") == 0);
    CHECK_EQ_U(ui.row_count, 20u);

    /* 反向按一次左键直接循环到最后一本（slot 19），打开翻页并持久化进度 */
    press_click(KEY_PREV);
    CHECK_EQ_U(mp_app_state.sel, 19u);
    CHECK(mp_app_view(&ui));
    CHECK(ui.row[MP_LIST_ROWS - 1].label != NULL &&
          strcmp(ui.row[MP_LIST_ROWS - 1].label, "Book_19") == 0);
    press_ok_single();
    CHECK_EQ_U(mp_app_state.screen, MP_SCREEN_READ);
    CHECK_EQ_U(mp_app_state.book_idx, 19u);
    press_click(KEY_NEXT);
    uint32_t saved_pos19 = mp_app_state.char_pos;
    CHECK(saved_pos19 > 0u);
    mp_app_flush_progress();
    uint32_t loaded_pos19 = 0;
    CHECK(mp_progress_load(19u, &loaded_pos19));
    CHECK_EQ_U(loaded_pos19, saved_pos19);

    /* 退出阅读并删除中间槽 10，随后上传新书应复用槽 10 而不增长容量 */
    press_click(KEY_MENU); /* READ -> SELECT */
    uint8_t del10[2] = {10u, 0u};
    sim_hal_ble_clear_tx();
    ble_send(MP_OP_BOOK_DELETE, del10, 2);
    CHECK_EQ_U(sim_hal_ble_tx_at(0)->data[1], MP_ST_OK);
    CHECK_EQ_U(mp_store_count_done(), 19u);
    uint16_t reused = ble_upload_book("Reused_10", (const uint8_t *)sample_long,
                                      sample_len, MP_ENC_ASCII);
    CHECK_EQ_U(reused, 10u);
    CHECK_EQ_U(mp_store_slot_capacity(), 24u);

    /* 删除末尾槽 16..19 后执行 0x1A 压缩，目录容量应从 24 收缩回 16 */
    for (uint16_t s = 16u; s < 20u; s++) {
        uint8_t dp[2] = {(uint8_t)(s & 0xFFu), (uint8_t)(s >> 8)};
        sim_hal_ble_clear_tx();
        ble_send(MP_OP_BOOK_DELETE, dp, 2);
        CHECK_EQ_U(sim_hal_ble_tx_at(0)->data[1], MP_ST_OK);
    }
    sim_hal_ble_clear_tx();
    ble_send(MP_OP_CATALOG_COMPACT, NULL, 0);
    CHECK_EQ_U(sim_hal_ble_tx_at(0)->data[1], MP_ST_OK);
    CHECK_EQ_U(mp_store_count_done(), 16u);
    CHECK_EQ_U(mp_store_slot_capacity(), 16u);

    /* 仅由设备剩余空间限制：空间耗尽时返回 MP_ST_NO_SPACE，且不无谓扩容 */
    sim_hal_set_fat_space(64ULL * 1024 * 1024, 32ULL * 1024);
    sim_hal_ble_clear_tx();
    ble_send(MP_OP_BOOK_BEGIN, ok_hdr, 9);
    CHECK_EQ_U(sim_hal_ble_tx_at(0)->data[1], MP_ST_NO_SPACE);
    CHECK_EQ_U(mp_store_slot_capacity(), 16u);
    sim_hal_shutdown();
}

/* ------------------------------------------------------------------ *
 * T05：锁屏图上传（0x30..0x32）、160 B 分块回读（0x33）与 y+6 像素映射
 * ------------------------------------------------------------------ */
static void test_t05_lock_image_upload_readback_and_mapping(void)
{
    sim_hal_init();
    mp_app_boot();
    sim_hal_ble_connect(true, 247);

    /* 构造 4000 B 锁屏图：前 6 行全黑（设备屏幕应裁切掉），可见区 y=6..127 画棋盘格。 */
    uint8_t img[MP_BLE_LOCK_BYTES];
    memset(img, 0, sizeof(img));
    for (int sy = 0; sy < 128; sy++) {
        for (int x = 0; x < 250; x++) {
            int black = (sy < 6) ? 1 : (((x / 8) + ((sy - 6) / 8)) & 1);
            if (black) {
                img[(sy >> 3) * 250 + x] |= (uint8_t)(1u << (sy & 7));
            }
        }
    }

    uint8_t lb[3] = {
        (uint8_t)((MP_BLE_LOCK_BYTES >> 16) & 0xFFu),
        (uint8_t)((MP_BLE_LOCK_BYTES >> 8) & 0xFFu),
        (uint8_t)(MP_BLE_LOCK_BYTES & 0xFFu),
    };
    sim_hal_ble_clear_tx();
    ble_send(MP_OP_LOCK_BEGIN, lb, 3);
    CHECK_EQ_U(sim_hal_ble_tx_at(0)->data[1], MP_ST_OK);

    for (uint32_t off = 0; off < MP_BLE_LOCK_BYTES; off += 200u) {
        uint8_t pkt[202];
        pkt[0] = (uint8_t)(off >> 8);
        pkt[1] = (uint8_t)(off & 0xFFu);
        memcpy(pkt + 2, img + off, 200u);
        sim_hal_ble_clear_tx();
        ble_send(MP_OP_LOCK_DATA, pkt, 202u);
        CHECK_EQ_U(sim_hal_ble_tx_at(0)->data[1], MP_ST_OK);
    }

    sim_hal_ble_clear_tx();
    ble_send(MP_OP_LOCK_END, NULL, 0);
    CHECK_EQ_U(sim_hal_ble_tx_at(0)->data[1], MP_ST_OK);
    CHECK(mp_store_lock_valid());

    /* 分块 160 B 回读 0x33 并逐字节核对 */
    uint8_t rb[MP_BLE_LOCK_BYTES];
    for (uint32_t off = 0; off < MP_BLE_LOCK_BYTES; off += MP_BLE_LOCK_CHUNK) {
        uint8_t req[2] = { (uint8_t)(off >> 8), (uint8_t)(off & 0xFFu) };
        sim_hal_ble_clear_tx();
        ble_send(MP_OP_LOCK_READ, req, 2);
        CHECK_EQ_U(sim_hal_ble_tx_count(), 1u);
        const sim_ble_pkt_t *p = sim_hal_ble_tx_at(0);
        uint32_t expect_n = MP_BLE_LOCK_BYTES - off;
        if (expect_n > MP_BLE_LOCK_CHUNK) {
            expect_n = MP_BLE_LOCK_CHUNK;
        }
        CHECK_EQ_U(p->len, (uint16_t)(3u + expect_n));
        CHECK_EQ_U(p->data[0], MP_OP_LOCK_READ);
        memcpy(rb + off, p->data + 3, expect_n);
    }
    CHECK(memcmp(rb, img, MP_BLE_LOCK_BYTES) == 0);

    /* 越界回读返回 0xFF 0xFF 结束标记 */
    uint8_t eof_req[2] = { 0x0F, 0xA0 }; /* 4000 */
    sim_hal_ble_clear_tx();
    ble_send(MP_OP_LOCK_READ, eof_req, 2);
    CHECK_EQ_U(sim_hal_ble_tx_at(0)->data[1], MP_ST_LOCK_END_OFF);

    /* 切换锁屏展示为图片并长按 OK 锁屏，逐像素核对 sy = y + 6 精确映射 */
    mp_settings.lock_display = MP_LOCK_DISP_IMAGE;
    press_long(KEY_OK);
    CHECK_EQ_U(mp_app_state.screen, MP_SCREEN_LOCK);
    const uint8_t *fb = sim_hal_last_fb();
    for (int y = 0; y < MP_FB_H; y++) {
        for (int x = 0; x < MP_FB_W; x++) {
            uint8_t b = img[(((y + 6) >> 3) * 250) + x];
            int expect_black = (b >> ((y + 6) & 7)) & 1;
            CHECK_EQ_U(fb_px_black(fb, x, y), expect_black);
        }
    }

    /* 锁屏状态下短按其他键弹出解锁提示页；按住 OK 1.2 秒松手不解锁；按住满 2 秒无需松手立刻解锁 */
    press_click(KEY_NEXT);
    CHECK_EQ_U(mp_app_state.lock_prompt, 1u);
    hold_lock_ok(1200u);
    CHECK_EQ_U(mp_app_state.screen, MP_SCREEN_LOCK);
    hold_lock_ok(MP_LOCK_UNLOCK_MS);
    CHECK_EQ_U(mp_app_state.screen, MP_SCREEN_CLOCK);
    CHECK_EQ_U(mp_app_state.lock_prompt, 0u);
    sim_hal_shutdown();
}

/* ------------------------------------------------------------------ *
 * T06：自动锁屏熄灭背光 + 解锁恢复背光 + 快捷背光浮窗 + 蓝牙省电自动关闭
 * ------------------------------------------------------------------ */
static void test_t06_auto_lock_backlight_and_ble_power_save(void)
{
    sim_hal_init();
    sim_hal_set_backlight_hw(true);
    mp_app_boot();

    /* 先上传一本书并进入阅读页 */
    sim_hal_ble_connect(true, 247);
    const uint8_t text[] = "Backlight test page one content for auto lock test.\n";
    (void)ble_upload_book("BL Test", text, (uint32_t)(sizeof(text) - 1u), MP_ENC_ASCII);
    sim_hal_ble_connect(false, 247);

    press_click(KEY_NEXT); /* CLOCK -> SELECT */
    press_ok_single();     /* SELECT -> READ */
    CHECK_EQ_U(mp_app_state.screen, MP_SCREEN_READ);

    /* 长按胶囊菜单键弹快捷背光浮窗 -> 调到 2 档 -> 确认保存 */
    press_long(KEY_FUNC);
    CHECK_EQ_U(mp_app_state.bl_popup, 1u);
    press_click(KEY_NEXT);
    press_click(KEY_NEXT);
    CHECK_EQ_U(sim_hal_backlight_hw_level(), 2u);
    press_ok_single();
    CHECK_EQ_U(mp_app_state.bl_popup, 0u);
    CHECK_EQ_U(mp_settings.backlight_level, 2u);
    CHECK(!sim_hal_backlight_is_suspended());

    /* 再测试浮窗取消：调到 3 档后按取消键，硬件应恢复 2 档 */
    press_long(KEY_FUNC);
    CHECK_EQ_U(mp_app_state.bl_popup, 1u);
    press_click(KEY_NEXT);
    CHECK_EQ_U(sim_hal_backlight_hw_level(), 3u);
    press_click(KEY_MENU);
    CHECK_EQ_U(mp_app_state.bl_popup, 0u);
    CHECK_EQ_U(sim_hal_backlight_hw_level(), 2u);
    CHECK_EQ_U(mp_settings.backlight_level, 2u);

    /* 设定锁屏超时为 60 s（idx=0），无操作推进 61 秒触发自动锁屏 */
    mp_settings.sleep_timeout_idx = 0;
    uint32_t susp_before = sim_hal_backlight_suspend_calls();
    advance_ticks(61000u, 1000u);
    CHECK_EQ_U(mp_app_state.screen, MP_SCREEN_LOCK);
    /* 核心断言：自动锁屏必须熄灭背光（调用 mp_backlight_suspend）！ */
    CHECK(sim_hal_backlight_suspend_calls() > susp_before);
    CHECK(sim_hal_backlight_is_suspended());

    /* 锁屏满 2 分钟无 BLE 通信应自动关闭蓝牙 */
    CHECK_EQ_U(mp_ble_running(), 1u);
    advance_ticks(MP_BLE_LOCK_OFF_MS + 500u, 1000u);
    CHECK_EQ_U(mp_ble_running(), 0u);
    CHECK_EQ_U(mp_settings.ble_enabled, 0u);

    /* 在锁屏关蓝牙后进入深睡前（2 秒宽限内）按住 OK 满 2 秒立即解锁（期间不误进深睡）：
     * 断言回到 READ 页，并且背光自动恢复为 2 档！ */
    hold_lock_ok(MP_LOCK_UNLOCK_MS);
    CHECK_EQ_U(mp_app_state.screen, MP_SCREEN_READ);
    CHECK(!sim_hal_backlight_is_suspended());
    CHECK_EQ_U(sim_hal_backlight_hw_level(), 2u);
    sim_hal_shutdown();
}

/* ------------------------------------------------------------------ *
 * T07：锁屏态 BLE 安全白名单隔离（只读放行，写入与 OTA 严厉拦截）
 * ------------------------------------------------------------------ */
static void test_t07_lock_ble_whitelist_security(void)
{
    sim_hal_init();
    setup_dual_slot_flash(1u, 44u, 0xFFFFFFFFu);
    mp_app_boot();
    sim_hal_ble_connect(true, 247);

    /* 先上传一本书，然后锁屏 */
    const uint8_t text[] = "Secret book body.\n";
    (void)ble_upload_book("Secret", text, (uint32_t)(sizeof(text) - 1u), MP_ENC_ASCII);
    press_long(KEY_OK);
    CHECK_EQ_U(mp_app_state.screen, MP_SCREEN_LOCK);

    /* 开启最严写保护（0 个允许区间）：锁屏态下不允许对 Flash 写入任何 1 字节 */
    sim_hal_flash_set_write_guard(NULL, 0);

    /* 白名单只读命令必须放行：0x14 / 0x19 / 0x34 / 0x50(含 locked=1) */
    sim_hal_ble_clear_tx();
    ble_send(MP_OP_BOOK_COUNT, NULL, 0);
    CHECK_EQ_U(sim_hal_ble_tx_at(0)->data[0], MP_OP_BOOK_COUNT);
    CHECK_EQ_U(sim_hal_ble_tx_at(0)->data[1], 1u);

    sim_hal_ble_clear_tx();
    ble_send(MP_OP_OTA_INFO, NULL, 0);
    CHECK_EQ_U(sim_hal_ble_tx_at(0)->data[0], MP_OP_OTA_INFO);
    CHECK_EQ_U(sim_hal_ble_tx_at(0)->data[1], MP_ST_OK);
    CHECK((sim_hal_ble_tx_at(0)->data[8] & (1u << 3)) != 0); /* locked bit = 1 */

    /* 危险写命令全部拦截 */
    uint8_t del0 = 0;
    sim_hal_ble_clear_tx();
    ble_send(MP_OP_BOOK_DELETE, &del0, 1);
    CHECK_EQ_U(sim_hal_ble_tx_at(0)->data[1], MP_ST_FAIL);
    CHECK_EQ_U(mp_store_count_done(), 1u); /* 书籍绝不能被删 */

    uint8_t ota_beg[36];
    memset(ota_beg, 0, sizeof(ota_beg));
    ota_beg[1] = 0x02; /* 128 KB */
    sim_hal_ble_clear_tx();
    ble_send(MP_OP_OTA_BEGIN, ota_beg, 36);
    CHECK_EQ_U(sim_hal_ble_tx_at(0)->data[0], MP_OP_OTA_BEGIN);
    CHECK_EQ_U(sim_hal_ble_tx_at(0)->data[1], MP_ST_OTA_LOCKED);

    uint8_t lock_beg[3] = {0, 0x0F, 0xA0};
    sim_hal_ble_clear_tx();
    ble_send(MP_OP_LOCK_BEGIN, lock_beg, 3);
    CHECK_EQ_U(sim_hal_ble_tx_at(0)->data[1], MP_ST_FAIL);

    /* 仍保持锁屏且 Flash 零违规写入 */
    CHECK_EQ_U(mp_app_state.screen, MP_SCREEN_LOCK);
    CHECK_EQ_U(sim_hal_flash_guard_violations(), 0u);
    sim_hal_shutdown();
}

/* ------------------------------------------------------------------ *
 * T08：跨深睡全链路（真双进程冷启动 + 短按误触拒醒 + 长按唤醒恢复阅读与精准走时）
 * ------------------------------------------------------------------ */
static int run_child_step(void (*fn)(const char *rtc_file), const char *rtc_file)
{
    pid_t pid = fork();
    CHECK(pid >= 0);
    if (pid == 0) {
        fn(rtc_file);
        _exit(0);
    }
    int status = 0;
    CHECK(waitpid(pid, &status, 0) == pid);
    return (WIFEXITED(status) && WEXITSTATUS(status) == 0) ? 0 : -1;
}

static uint32_t s_t08_saved_pos;

static void t08_boot1_enter_deep_sleep(const char *rtc_file)
{
    sim_hal_init();
    mp_app_boot();
    sim_hal_ble_connect(true, 247);

    /* 1) 对时为 2026-02-28 23:58:30（周六，非闰年 2 月底跨月边界） */
    /* 2026-02-28 23:58:30 UTC = 1772323110 */
    mp_time_t t = {
        .unix = 1772323110u,
        .year = 2026,
        .month = 2,
        .day = 28,
        .week = 6,
        .hour = 23,
        .min = 58,
        .sec = 30,
    };
    sim_hal_ble_inject_time(&t);
    advance_ticks(100, 100);
    CHECK_EQ_U(mp_app_state.month, 2u);
    CHECK_EQ_U(mp_app_state.day, 28u);
    CHECK_EQ_U(mp_app_state.hour, 23u);
    CHECK_EQ_U(mp_app_state.min, 58u);

    /* 2) 上传一本足够长的书并翻到第 3 页 */
    char body[4096];
    size_t pos = 0;
    for (int i = 0; i < 40; i++) {
        int n = snprintf(body + pos, sizeof(body) - pos,
                         "Page content line %02d for deep sleep persistence check.\n", i);
        CHECK(n > 0 && (size_t)n < sizeof(body) - pos);
        pos += (size_t)n;
    }
    (void)ble_upload_book("DeepSleepBook", (const uint8_t *)body, (uint32_t)pos, MP_ENC_ASCII);
    sim_hal_ble_connect(false, 247);

    press_click(KEY_NEXT); /* SELECT */
    press_ok_single();     /* READ */
    press_click(KEY_NEXT);
    press_click(KEY_NEXT);
    uint32_t saved_pos = mp_app_state.char_pos;
    CHECK(saved_pos > 0u);
    /* 把 saved_pos 写入沙盒标记文件供下一进程核对 */
    FILE *fp = fopen("./t08_pos.txt", "wb");
    CHECK(fp != NULL);
    fprintf(fp, "%u\n", (unsigned)saved_pos);
    fclose(fp);

    /* 3) 关蓝牙 -> 长按 OK 锁屏 -> 等 10 秒进入真深睡 */
    mp_settings.ble_enabled = 0;
    mp_ble_stop();
    press_long(KEY_OK);
    CHECK_EQ_U(mp_app_state.screen, MP_SCREEN_LOCK);

    int trap = setjmp(*sim_hal_trap_buf());
    if (trap == 0) {
        sim_hal_arm_trap(true);
        advance_ticks(MP_LOCK_SLEEP_GRACE_MS + 2000u, 500u);
    }
    sim_hal_arm_trap(false);
    CHECK_EQ_U(trap, SIM_TRAP_DEEP_SLEEP);

    /* 4) 审计深睡前硬件电源锁存与 XTAL32K/RTC_PERIPH 保持配置 */
    const sim_sleep_audit_t *sa = sim_hal_sleep_audit();
    CHECK(sa->deep_sleep_entered);
    CHECK(sa->rtc_periph_kept);
    CHECK(sa->rtc_slow_mem_kept);
    CHECK(sa->xtal32k_kept);
    CHECK(!sa->deep_sleep_hold);
    CHECK_EQ_U(sa->rtc_pad_held_mask, 0ULL);
    CHECK(sa->bl_deep_slept);
    CHECK(sa->vbat_en_held && sa->vbat_en_level == 0);
    CHECK(sa->sd_pwr_held && sa->sd_pwr_level == 1);
    uint64_t want_sd_float = (1ULL << BOARD_SD_PIN_CLK) | (1ULL << BOARD_SD_PIN_CMD) |
                             (1ULL << BOARD_SD_PIN_D0)  | (1ULL << BOARD_SD_PIN_D1)  |
                             (1ULL << BOARD_SD_PIN_D2)  | (1ULL << BOARD_SD_PIN_D3);
    CHECK_EQ_U(sa->floated_pins_mask & want_sd_float, want_sd_float);
    CHECK_EQ_U(sa->ext1_mask,
               (1ULL << BOARD_KEY_OK_GPIO) | (1ULL << BOARD_CHARGE_DET_GPIO));
    CHECK(sim_hal_display_asleep());

    /* 保存 RTC Slow Memory 段与墙上时钟到文件 */
    CHECK(sim_hal_rtc_save(rtc_file) == 0);
    sim_hal_shutdown();
}

static void t08_boot2a_short_press_rejected(const char *rtc_file)
{
    /* 全新进程模拟芯片冷唤醒：RAM 全零，仅加载 RTC Slow Memory。 */
    sim_hal_init();
    CHECK(sim_hal_rtc_load(rtc_file) == 0);
    /* 模拟深睡过去 2 小时 3 分钟 */
    sim_hal_sleep_advance_ms((2ULL * 3600ULL + 3ULL * 60ULL) * 1000ULL);
    sim_hal_set_wakeup(1u << ESP_SLEEP_WAKEUP_EXT1, 1ULL << BOARD_KEY_OK_GPIO);
    /* 短按或不足 2 秒误触：OK 仅按 1000 ms（< MP_WAKE_HOLD_MS=1650 ms） */
    sim_hal_set_ok_hold_ms(1000u);

    int trap = setjmp(*sim_hal_trap_buf());
    if (trap == 0) {
        sim_hal_arm_trap(true);
        mp_app_early_wake_check();
    }
    sim_hal_arm_trap(false);
    /* 必须直接回到深睡，未挂载存储/屏幕，且保留 RTC_PERIPH 与充电唤醒脚！ */
    CHECK_EQ_U(trap, SIM_TRAP_DEEP_SLEEP);
    CHECK_EQ_U(sim_hal_display_frames(), 0u);
    const sim_sleep_audit_t *sa = sim_hal_sleep_audit();
    CHECK(sa->rtc_periph_kept);
    CHECK_EQ_U(sa->ext1_mask,
               (1ULL << BOARD_KEY_OK_GPIO) | (1ULL << BOARD_CHARGE_DET_GPIO));
    sim_hal_shutdown();
}

static void t08_boot2b_long_hold_wake_and_clock(const char *rtc_file)
{
    /* 全新进程再次唤醒：持续按住 OK 满 2 秒（在 early_wake_check 中按满 1650 ms，无需松手即唤醒）。 */
    sim_hal_init();
    CHECK(sim_hal_rtc_load(rtc_file) == 0);
    /* 推进 2 小时 3 分钟（从 2026-02-28 23:58:42+ -> 2026-03-01 02:01+，周日） */
    sim_hal_sleep_advance_ms((2ULL * 3600ULL + 3ULL * 60ULL) * 1000ULL);
    sim_hal_set_wakeup(1u << ESP_SLEEP_WAKEUP_EXT1, 1ULL << BOARD_KEY_OK_GPIO);
    sim_hal_set_ok_hold_ms(3000u); /* 按满 1650 ms 时仍未松手即刻唤醒 */

    mp_app_early_wake_check();
    mp_app_boot();

    /* 读回上次阅读页偏移 */
    FILE *fp = fopen("./t08_pos.txt", "rb");
    CHECK(fp != NULL);
    unsigned want_pos = 0;
    CHECK(fscanf(fp, "%u", &want_pos) == 1);
    fclose(fp);
    s_t08_saved_pos = want_pos;

    /* 断言直接恢复到 READ 屏与精确进度，且跨月跨午夜走时完全准确！ */
    CHECK_EQ_U(mp_app_state.screen, MP_SCREEN_READ);
    CHECK_EQ_U(mp_app_state.char_pos, s_t08_saved_pos);
    CHECK_EQ_U(mp_app_state.month, 3u);
    CHECK_EQ_U(mp_app_state.day, 1u);
    CHECK_EQ_U(mp_app_state.week, 7u); /* 周日 */
    CHECK_EQ_U(mp_app_state.hour, 2u);
    CHECK(mp_app_state.min == 1u || mp_app_state.min == 2u);
    sim_hal_shutdown();
}

static void test_t08_deep_sleep_rtc_and_miswake(void)
{
    const char *rtc_file = "./t08_rtc.bin";
    CHECK(run_child_step(t08_boot1_enter_deep_sleep, rtc_file) == 0);
    CHECK(run_child_step(t08_boot2a_short_press_rejected, rtc_file) == 0);
    CHECK(run_child_step(t08_boot2b_long_hold_wake_and_clock, rtc_file) == 0);
}

/* ------------------------------------------------------------------ *
 * T09：闰年日历进位（2024-02-29）+ 手动时间设置（TIME_SET -> TIME_DONE）
 * ------------------------------------------------------------------ */
static void test_t09_leap_year_and_time_set(void)
{
    sim_hal_init();
    mp_app_boot();

    /* 2024-02-28 23:59:30 UTC = 1709164770 (周三) */
    mp_time_t t = {
        .unix = 1709164770u,
        .year = 2024,
        .month = 2,
        .day = 28,
        .week = 3,
        .hour = 23,
        .min = 59,
        .sec = 30,
    };
    sim_hal_ble_inject_time(&t);
    advance_ticks(100, 100);
    CHECK_EQ_U(mp_app_state.month, 2u);
    CHECK_EQ_U(mp_app_state.day, 28u);

    /* 推进 40 秒跨午夜进入闰日 2024-02-29 00:00（周四） */
    advance_ticks(40000u, 1000u);
    CHECK_EQ_U(mp_app_state.month, 2u);
    CHECK_EQ_U(mp_app_state.day, 29u);
    CHECK_EQ_U(mp_app_state.week, 4u);
    CHECK_EQ_U(mp_app_state.hour, 0u);
    CHECK_EQ_U(mp_app_state.min, 0u);

    /* 推进 24 小时跨入 2024-03-01（周五）；期间会触发自动锁屏，先解锁回时钟 */
    sim_hal_advance_ms(86400ULL * 1000ULL);
    tick_now();
    CHECK_EQ_U(mp_app_state.month, 3u);
    CHECK_EQ_U(mp_app_state.day, 1u);
    CHECK_EQ_U(mp_app_state.week, 5u);
    if (mp_app_state.screen == MP_SCREEN_LOCK) {
        hold_lock_ok(MP_LOCK_UNLOCK_MS);
    }
    CHECK_EQ_U(mp_app_state.screen, MP_SCREEN_CLOCK);

    /* 按键走完设置页 -> 时间设置 5 步 -> TIME_DONE -> 自动返回 SETTINGS */
    press_click(KEY_PREV); /* CLOCK -> SETTINGS */
    for (int i = 0; i < 4; i++) {
        press_click(KEY_NEXT); /* index 4 = 时间设置 */
    }
    press_ok_single();
    CHECK_EQ_U(mp_app_state.screen, MP_SCREEN_TIME_SET);
    for (int step = 0; step < 5; step++) {
        press_click(KEY_NEXT);
        press_ok_single();
    }
    CHECK_EQ_U(mp_app_state.screen, MP_SCREEN_TIME_DONE);
    advance_ticks(2000u, 200u);
    CHECK_EQ_U(mp_app_state.screen, MP_SCREEN_SETTINGS);
    sim_hal_shutdown();
}

/* ------------------------------------------------------------------ *
 * T10：低电自动关机保护（未充电 < 3% 进深睡，充电中绝不关机）
 * ------------------------------------------------------------------ */
static void test_t10_low_battery_shutdown(void)
{
    sim_hal_init();
    sim_hal_set_battery(20, 3650, true, false);
    mp_app_boot();

    /* 电量掉到 2% 且未充电 -> tick 立即全刷低电屏并进 EXT1(充电脚) 深睡 */
    sim_hal_set_battery(2, 3505, true, false);
    int trap = setjmp(*sim_hal_trap_buf());
    if (trap == 0) {
        sim_hal_arm_trap(true);
        advance_ticks(2000u, 500u);
    }
    sim_hal_arm_trap(false);
    CHECK_EQ_U(trap, SIM_TRAP_DEEP_SLEEP);
    CHECK_EQ_U(mp_app_state.screen, MP_SCREEN_LOW_BATT);
    const sim_sleep_audit_t *sa = sim_hal_sleep_audit();
    CHECK_EQ_U(sa->ext1_mask,
               (1ULL << BOARD_CHARGE_DET_GPIO) | (1ULL << BOARD_KEY_OK_GPIO));
    sim_hal_shutdown();
}

/* ------------------------------------------------------------------ *
 * T11：原生 BLE 原地 OTA 全链路真包实测（支持真实 build/miaopaper_e2_e2s.bin）
 * ------------------------------------------------------------------ */
static __attribute__((noinline)) int trap_ble_send(uint8_t op,
                                                   const void *payload,
                                                   uint8_t len)
{
    int trap = setjmp(*sim_hal_trap_buf());
    if (trap == 0) {
        sim_hal_arm_trap(true);
        ble_send(op, payload, len);
    }
    sim_hal_arm_trap(false);
    return trap;
}

static __attribute__((noinline)) uint8_t *load_ota_firmware_image(
    size_t *out_len, bool *is_real_bin)
{
    const char *env_path = getenv("MP_SIM_OTA_BIN");
    const char *candidates[2] = { env_path, "../build/miaopaper_e2_e2s.bin" };
    for (int i = 0; i < 2; i++) {
        const char *p = candidates[i];
        if (!p || !p[0]) {
            continue;
        }
        FILE *f = fopen(p, "rb");
        if (!f) {
            continue;
        }
        if (fseek(f, 0, SEEK_END) == 0) {
            long sz = ftell(f);
            if (sz >= (long)MP_OTA_MIN_IMAGE_SIZE && sz <= (long)MP_OTA_MAX_IMAGE_SIZE &&
                fseek(f, 0, SEEK_SET) == 0) {
                uint8_t *buf = (uint8_t *)malloc((size_t)sz);
                if (buf && fread(buf, 1, (size_t)sz, f) == (size_t)sz) {
                    fclose(f);
                    *out_len = (size_t)sz;
                    *is_real_bin = true;
                    return buf;
                }
                free(buf);
            }
        }
        fclose(f);
    }

    size_t cap = 128 * 1024u;
    uint8_t *buf = (uint8_t *)malloc(cap);
    CHECK(buf != NULL);
    size_t n = sim_build_esp_image(buf, cap, CONFIG_IDF_FIRMWARE_CHIP_ID,
                                   "miaopaper_e2_e2s", "1.2.1-sim",
                                   96 * 1024u, true);
    CHECK(n >= MP_OTA_MIN_IMAGE_SIZE);
    *out_len = n;
    *is_real_bin = false;
    return buf;
}

static void test_t11_native_ble_ota_full_real_image(void)
{
    sim_hal_init();
    setup_dual_slot_flash(1u, 43u, 44u);
    sim_hal_set_battery(80, 4000, true, false);
    mp_app_boot();
    sim_hal_ble_connect(true, 247);

    /* 备份禁写区，用于 OTA 完成后逐字节比对确保零污染。 */
    uint8_t *fl = sim_hal_flash_raw();
    uint8_t snap_boot_part_nvs_ota_phy[0x12000u];
    memcpy(snap_boot_part_nvs_ota_phy, fl, sizeof(snap_boot_part_nvs_ota_phy));
    uint8_t *snap_stock_slot = (uint8_t *)malloc(SIM_PART_OTA_SIZE);
    uint8_t *snap_stock_font = (uint8_t *)malloc(SIM_STOCK_FONT_SIZE);
    CHECK(snap_stock_slot && snap_stock_font);
    memcpy(snap_stock_slot, fl + SIM_PART_OTA0_ADDR, SIM_PART_OTA_SIZE);
    memcpy(snap_stock_font, fl + SIM_STOCK_FONT_ADDR, SIM_STOCK_FONT_SIZE);

    /* 开启硬件级写保护红线：只允许写 0x530000+ 暂存区和当前第三方槽 ota_1。 */
    const sim_flash_range_t allow[2] = {
        { MP_OTA_STAGE_HDR_ADDR, MP_OTA_STAGE_DATA_ADDR + SIM_PART_OTA_SIZE },
        { SIM_PART_OTA1_ADDR, SIM_PART_OTA1_ADDR + SIM_PART_OTA_SIZE },
    };
    sim_hal_flash_set_write_guard(allow, 2);

    /* 1) 0x50 信息查询 */
    sim_hal_ble_clear_tx();
    ble_send(MP_OP_OTA_INFO, NULL, 0);
    CHECK_EQ_U(sim_hal_ble_tx_count(), 1u);
    const sim_ble_pkt_t *info = sim_hal_ble_tx_at(0);
    CHECK_EQ_U(info->data[0], MP_OP_OTA_INFO);
    CHECK_EQ_U(info->data[1], MP_ST_OK);
    CHECK_EQ_U(info->data[2], MP_OTA_PROTO_VER);
    CHECK_EQ_U(info->data[7], 1u); /* running_slot = ota_1 */
    CHECK((info->data[8] & (1u << 1)) != 0); /* stage_ok = 1 */
    CHECK((info->data[8] & (1u << 2)) != 0); /* other_is_stock = 1 */
    CHECK((info->data[8] & (1u << 3)) == 0); /* locked = 0 */

    /* 2) 加载真实 build/miaopaper_e2_e2s.bin（若无则合成包） */
    size_t img_len = 0;
    bool is_real = false;
    uint8_t *img = load_ota_firmware_image(&img_len, &is_real);
    uint8_t sha[32];
    sim_sha256(img, img_len, sha);

    /* 3) 0x51 开启 OTA 会话 */
    uint8_t beg[36];
    beg[0] = (uint8_t)((img_len >> 24) & 0xFFu);
    beg[1] = (uint8_t)((img_len >> 16) & 0xFFu);
    beg[2] = (uint8_t)((img_len >> 8) & 0xFFu);
    beg[3] = (uint8_t)(img_len & 0xFFu);
    memcpy(beg + 4, sha, 32u);

    uint32_t frames_before = sim_hal_display_frames();
    sim_hal_ble_clear_tx();
    ble_send(MP_OP_OTA_BEGIN, beg, 36u);
    CHECK_EQ_U(sim_hal_ble_tx_count(), 1u);
    const sim_ble_pkt_t *br = sim_hal_ble_tx_at(0);
    CHECK_EQ_U(br->len, 6u);
    CHECK_EQ_U(br->data[0], MP_OP_OTA_BEGIN);
    CHECK_EQ_U(br->data[1], MP_ST_OK);
    uint16_t chunk_max = (uint16_t)(((uint16_t)br->data[2] << 8) | br->data[3]);
    CHECK(chunk_max >= 16u && chunk_max <= 240u);
    /* 设备端不切屏也不重绘（OTA 进度仅在 App 展示）。 */
    CHECK_EQ_U(mp_app_state.screen, MP_SCREEN_CLOCK);
    CHECK_EQ_U(sim_hal_display_frames(), frames_before);

    /* 4) 0x52 分片顺序发送，期间故意重发一次旧包验证幂等性与静默按键屏蔽 */
    size_t off = 0;
    bool tested_dup = false;
    while (off < img_len) {
        size_t take = img_len - off;
        if (take > chunk_max) {
            take = chunk_max;
        }
        uint8_t pkt[3 + 240];
        pkt[0] = (uint8_t)((off >> 16) & 0xFFu);
        pkt[1] = (uint8_t)((off >> 8) & 0xFFu);
        pkt[2] = (uint8_t)(off & 0xFFu);
        memcpy(pkt + 3, img + off, take);
        sim_hal_ble_clear_tx();
        ble_send(MP_OP_OTA_DATA, pkt, (uint8_t)(3u + take));
        off += take;

        if (!tested_dup && off >= 4096u) {
            tested_dup = true;
            /* 重发 offset=0 的旧包必须返回 MP_ST_OK 且水位仍是 off */
            uint8_t dup[3 + 64];
            dup[0] = dup[1] = dup[2] = 0;
            memcpy(dup + 3, img, 64u);
            sim_hal_ble_clear_tx();
            ble_send(MP_OP_OTA_DATA, dup, 67u);
            CHECK_EQ_U(sim_hal_ble_tx_count(), 1u);
            const sim_ble_pkt_t *dr = sim_hal_ble_tx_at(0);
            CHECK_EQ_U(dr->data[1], MP_ST_OK);
            uint32_t w = ((uint32_t)dr->data[2] << 24) | ((uint32_t)dr->data[3] << 16) |
                         ((uint32_t)dr->data[4] << 8) | dr->data[5];
            CHECK_EQ_U(w, off);

            /* 传输中按键必须静默屏蔽不切屏 */
            press_click(KEY_NEXT);
            CHECK_EQ_U(mp_app_state.screen, MP_SCREEN_CLOCK);
        }
    }
    CHECK_EQ_U(sim_hal_display_frames(), frames_before);

    /* 5) 0x53 整包 5 重校验 + IRAM 原地覆写当前第三方槽 + 软复位 */
    sim_hal_ble_clear_tx();
    int trap = trap_ble_send(MP_OP_OTA_END, NULL, 0);
    CHECK_EQ_U(trap, SIM_TRAP_RESTART_NOOS);

    /* 核验先回了 0x53 OK 通知再关外设进入 IRAM 刷写 */
    CHECK_EQ_U(sim_hal_ble_tx_count(), 1u);
    CHECK_EQ_U(sim_hal_ble_tx_at(0)->data[0], MP_OP_OTA_END);
    CHECK_EQ_U(sim_hal_ble_tx_at(0)->data[1], MP_ST_OK);
    CHECK_EQ_U(mp_ble_running(), 0u);
    CHECK(sim_hal_display_asleep());
    CHECK(sim_hal_backlight_is_suspended());

    /* 审计 IRAM 自刷写硬件操作完备性 */
    const sim_iram_audit_t *ia = sim_hal_iram_audit();
    CHECK(ia->intr_disabled);
    CHECK(ia->wdt_disabled[0] && ia->wdt_disabled[1] && ia->wdt_disabled[2]);
    CHECK((ia->cpu_reset_mask & (1u << 1)) != 0);
    CHECK((ia->cpu_stalled_mask & (1u << 1)) != 0);
    CHECK(ia->icache_disabled && ia->dcache_disabled);
    CHECK(ia->rom_flash_unlocked);
    uint32_t expect_sectors = ((uint32_t)img_len + 0xFFFu) / 0x1000u;
    CHECK_EQ_U(ia->rom_erase_count, expect_sectors);
    CHECK_EQ_U(ia->rom_write_count, expect_sectors);

    /* 终极字节级安全校验：运行槽 100% 匹配新包，全部禁写区 0 字节被改动！ */
    CHECK_EQ_U(sim_hal_flash_guard_violations(), 0u);
    CHECK(memcmp(fl + SIM_PART_OTA1_ADDR, img, img_len) == 0);
    CHECK(memcmp(fl, snap_boot_part_nvs_ota_phy, sizeof(snap_boot_part_nvs_ota_phy)) == 0);
    CHECK(memcmp(fl + SIM_PART_OTA0_ADDR, snap_stock_slot, SIM_PART_OTA_SIZE) == 0);
    CHECK(memcmp(fl + SIM_STOCK_FONT_ADDR, snap_stock_font, SIM_STOCK_FONT_SIZE) == 0);

    if (is_real) {
        printf("    (实测真实构建镜像 build/miaopaper_e2_e2s.bin, %zu B)\n", img_len);
    }
    free(img);
    free(snap_stock_slot);
    free(snap_stock_font);
    sim_hal_shutdown();
}

/* ------------------------------------------------------------------ *
 * T12：原生 BLE OTA 全套反向安全拦截矩阵
 * ------------------------------------------------------------------ */
static void test_t12_ota_security_rejections(void)
{
    sim_hal_init();
    setup_dual_slot_flash(1u, 43u, 44u);
    mp_app_boot();
    sim_hal_ble_connect(true, 247);

    uint8_t *fl = sim_hal_flash_raw();
    uint8_t *img = (uint8_t *)malloc(128 * 1024u);
    CHECK(img != NULL);
    size_t img_len = sim_build_esp_image(img, 128 * 1024u, CONFIG_IDF_FIRMWARE_CHIP_ID,
                                         "miaopaper_e2_e2s", "1.2.9", 72 * 1024u, true);
    uint8_t sha[32];
    sim_sha256(img, img_len, sha);

    /* 1) 未充电低电 15% 拒收；插电 15% 放行 */
    sim_hal_set_battery(15, 3600, true, false);
    CHECK_EQ_U(mp_ota_begin((uint32_t)img_len, sha, 15, true, false), MP_ST_OTA_LOW_BATT);
    CHECK_EQ_U(mp_ota_begin((uint32_t)img_len, sha, 15, true, true), MP_ST_OK);
    mp_ota_abort();

    /* 2) 暂存区被未知非 0xFF 数据占用 -> 拒绝擦除 */
    fl[MP_OTA_STAGE_HDR_ADDR] = 0x55; /* 破坏 "MPOTASTG" 魔数 */
    CHECK_EQ_U(mp_ota_begin((uint32_t)img_len, sha, 80, true, false), MP_ST_OTA_STAGE_DIRTY);
    memset(fl + MP_OTA_STAGE_HDR_ADDR, 0xFF, 0x1000u);
    memset(fl + MP_OTA_STAGE_DATA_ADDR, 0xFF, 0x20000u);

    /* 3) 乱序/跳跃偏移必须返回 MP_ST_OTA_SEQ，且不推进水位 */
    CHECK_EQ_U(mp_ota_begin((uint32_t)img_len, sha, 80, true, false), MP_ST_OK);
    uint32_t w = 0;
    CHECK_EQ_U(mp_ota_write(0, img, 128u, &w), MP_ST_OK);
    CHECK_EQ_U(w, 128u);
    CHECK_EQ_U(mp_ota_write(256u, img + 256u, 128u, &w), MP_ST_OTA_SEQ);
    CHECK_EQ_U(w, 128u);
    CHECK_EQ_U(mp_ota_write(64u, img + 64u, 128u, &w), MP_ST_OTA_SEQ);
    CHECK_EQ_U(w, 128u);
    mp_ota_abort();

    /* 4) 整包 SHA-256 故意传错 -> verify 拦截返回 MP_ST_OTA_SHA_MISMATCH */
    uint8_t bad_sha[32];
    memcpy(bad_sha, sha, 32u);
    bad_sha[0] ^= 0xFFu;
    CHECK_EQ_U(mp_ota_begin((uint32_t)img_len, bad_sha, 80, true, false), MP_ST_OK);
    CHECK_EQ_U(mp_ota_write(0, img, img_len, &w), MP_ST_OK);
    CHECK_EQ_U(mp_ota_verify_staged(80, true, false), MP_ST_OTA_SHA_MISMATCH);
    mp_ota_abort();

    /* 5) 内部 1 bit 篡改但重算外部 SHA-256 企图绕过 -> 第 2 关 esp_image_verify 拦截 */
    uint8_t *tampered = (uint8_t *)malloc(img_len);
    CHECK(tampered != NULL);
    memcpy(tampered, img, img_len);
    tampered[500] ^= 0x01u;
    sim_sha256(tampered, img_len, bad_sha);
    CHECK_EQ_U(mp_ota_begin((uint32_t)img_len, bad_sha, 80, true, false), MP_ST_OK);
    CHECK_EQ_U(mp_ota_write(0, tampered, img_len, &w), MP_ST_OK);
    CHECK_EQ_U(mp_ota_verify_staged(80, true, false), MP_ST_OTA_BAD_IMAGE);
    mp_ota_abort();
    free(tampered);

    /* 6) 错误芯片型号（ESP32=0x0000 冒充 ESP32-S3） -> 拦截 */
    size_t wrong_chip_len = sim_build_esp_image(img, 128 * 1024u, ESP_CHIP_ID_ESP32,
                                                "miaopaper_e2_e2s", "1.2.9", 72 * 1024u, true);
    sim_sha256(img, wrong_chip_len, bad_sha);
    CHECK_EQ_U(mp_ota_begin((uint32_t)wrong_chip_len, bad_sha, 80, true, false), MP_ST_OK);
    CHECK_EQ_U(mp_ota_write(0, img, wrong_chip_len, &w), MP_ST_OK);
    CHECK_EQ_U(mp_ota_verify_staged(80, true, false), MP_ST_OTA_BAD_IMAGE);
    mp_ota_abort();

    /* 7) 误传官方固件包（project_name = "paperang_e2"） -> 第 3 关拦截 */
    size_t stock_pkg_len = sim_build_esp_image(img, 128 * 1024u, CONFIG_IDF_FIRMWARE_CHIP_ID,
                                               "paperang_e2", "1.3.739", 72 * 1024u, true);
    sim_sha256(img, stock_pkg_len, bad_sha);
    CHECK_EQ_U(mp_ota_begin((uint32_t)stock_pkg_len, bad_sha, 80, true, false), MP_ST_OK);
    CHECK_EQ_U(mp_ota_write(0, img, stock_pkg_len, &w), MP_ST_OK);
    CHECK_EQ_U(mp_ota_verify_staged(80, true, false), MP_ST_OTA_BAD_IMAGE);
    mp_ota_abort();

    /* 8) 若当前运行槽为原厂 project_name，绝不允许自刷写 */
    sim_hal_flash_set_app_desc("paperang_e2", "1.3.739", "Nov 27 2026", "04:00:00");
    CHECK_EQ_U(mp_ota_begin((uint32_t)img_len, sha, 80, true, false), MP_ST_BAD_PARAM);
    sim_hal_flash_set_app_desc("miaopaper_e2_e2s", "1.2.0", "Nov 27 2026", "04:00:00");

    /* 9) BLE 0x54 中止会话恢复正常态 */
    sim_hal_set_battery(80, 4000, true, false);
    img_len = sim_build_esp_image(img, 128 * 1024u, CONFIG_IDF_FIRMWARE_CHIP_ID,
                                  "miaopaper_e2_e2s", "1.2.9", 72 * 1024u, true);
    sim_sha256(img, img_len, sha);
    uint8_t beg[36];
    beg[0] = (uint8_t)((img_len >> 24) & 0xFFu);
    beg[1] = (uint8_t)((img_len >> 16) & 0xFFu);
    beg[2] = (uint8_t)((img_len >> 8) & 0xFFu);
    beg[3] = (uint8_t)(img_len & 0xFFu);
    memcpy(beg + 4, sha, 32u);
    ble_send(MP_OP_OTA_BEGIN, beg, 36u);
    CHECK_EQ_U(mp_app_state.uploading, 1u);
    sim_hal_ble_clear_tx();
    ble_send(MP_OP_OTA_ABORT, NULL, 0);
    CHECK_EQ_U(sim_hal_ble_tx_at(0)->data[1], MP_ST_OK);
    CHECK_EQ_U(mp_app_state.uploading, 0u);

    free(img);
    sim_hal_shutdown();
}

/* ------------------------------------------------------------------ *
 * T13：恢复原厂固件（recovery.c + mp_app 武装确认门 + 双扇区 otadata 保护）
 * ------------------------------------------------------------------ */
static void test_t13_restore_stock_flow_and_safety(void)
{
    sim_hal_init();
    /* 当前跑 ota_1，otadata 扇区0=seq 44（指向 ota_1），扇区1=擦除态，ota_0 为原厂 */
    setup_dual_slot_flash(1u, 44u, 0xFFFFFFFFu);
    mp_app_boot();

    /* 只允许写 otadata 分区（0xF000..0x11000），严禁触碰任何 App 镜像/分区表！ */
    const sim_flash_range_t allow[1] = {
        { SIM_PART_OTADATA_ADDR, SIM_PART_OTADATA_ADDR + 0x2000u },
    };
    sim_hal_flash_set_write_guard(allow, 1);

    /* 进入设置 -> 第 6 项（恢复原厂固件） -> 单击 OK 进确认页 */
    press_click(KEY_PREV);
    for (int i = 0; i < 6; i++) {
        press_click(KEY_NEXT);
    }
    press_ok_single();
    CHECK_EQ_U(mp_app_state.screen, MP_SCREEN_RESTORE);
    CHECK_EQ_U(mp_app_state.restore_armed, 1u);
    CHECK_EQ_U(mp_app_state.restore_key_ok, 0u);

    /* 防误触门测试：未见过新的 OK DOWN 直接发 OK LONG，必须忽略不提交！ */
    mp_app_key(KEY_OK, KEY_EV_LONG, now32());
    CHECK_EQ_U(mp_app_state.screen, MP_SCREEN_RESTORE);

    /* 发出新的 OK DOWN + OK LONG -> 执行 recovery_stock_commit() 并等待 3 秒重启 */
    int trap = setjmp(*sim_hal_trap_buf());
    if (trap == 0) {
        sim_hal_arm_trap(true);
        press_long(KEY_OK);
    }
    sim_hal_arm_trap(false);
    CHECK_EQ_U(trap, SIM_TRAP_RESTART);
    CHECK_EQ_U(sim_hal_flash_guard_violations(), 0u);

    /* 校验 otadata：原扇区 0（seq=44）完好保留作回退，扇区 1 写入 seq=45（指向 ota_0） */
    const uint8_t *fl = sim_hal_flash_raw();
    esp_ota_select_entry_t e0, e1;
    memcpy(&e0, fl + SIM_PART_OTADATA_ADDR, sizeof(e0));
    memcpy(&e1, fl + SIM_PART_OTADATA_ADDR + SIM_FLASH_SECTOR, sizeof(e1));
    CHECK_EQ_U(e0.ota_seq, 44u);
    CHECK_EQ_U(e0.crc, bootloader_common_ota_select_crc(&e0));
    CHECK_EQ_U(e1.ota_seq, 45u);
    CHECK_EQ_U(e1.crc, bootloader_common_ota_select_crc(&e1));
    CHECK_EQ_U(e1.ota_state, ESP_OTA_IMG_UNDEFINED);
    CHECK_EQ_U(esp_ota_get_boot_partition()->address, SIM_PART_OTA0_ADDR);
    sim_hal_shutdown();
}

static void test_t13b_restore_stock_rejects_corrupt_slot(void)
{
    sim_hal_init();
    setup_dual_slot_flash(1u, 44u, 0xFFFFFFFFu);
    /* 破坏原厂槽 1 字节 -> probe 必须拒绝且 otadata 零写入 */
    sim_hal_flash_raw()[SIM_PART_OTA0_ADDR + 512u] ^= 0x80u;
    sim_hal_flash_set_write_guard(NULL, 0);

    char detail[64];
    CHECK_EQ_U(recovery_stock_probe(detail, sizeof(detail)), RECOVERY_ERR_NO_STOCK);
    CHECK_EQ_U(recovery_stock_commit(detail, sizeof(detail)), RECOVERY_ERR_NO_STOCK);
    CHECK_EQ_U(sim_hal_flash_guard_violations(), 0u);
    sim_hal_shutdown();
}

/* ------------------------------------------------------------------ *
 * T14：存储掉电恢复（catalog.bak 恢复 + TOC 缓存 CRC 失效自动重建）
 * ------------------------------------------------------------------ */
static void test_t14_store_power_loss_and_toc_invalidation(void)
{
    sim_hal_init();
    mp_app_boot();
    sim_hal_ble_connect(true, 247);

    const char *v1 =
        "\xB5\xDA\xD2\xBB\xD5\xC2 \xBF\xAA\xC6\xAA\n"
        "Body text version one.\n";
    uint16_t slot = ble_upload_book("RecoverTest", (const uint8_t *)v1,
                                    (uint32_t)strlen(v1), MP_ENC_GB2312);
    uint16_t cnt = 0;
    CHECK(mp_toc_ensure(slot, (uint32_t)strlen(v1), MP_ENC_GB2312, &cnt) == 0);
    CHECK_EQ_U(cnt, 1u);

    /* 场景 A：写窗口内 rename(bin, bak) 后掉电，catalog.bin 缺失但 catalog.bak 完整 */
    FILE *src = fopen("./mp/catalog.bin", "rb");
    CHECK(src != NULL);
    uint8_t cat_bak[8 + MP_SLOT_CHUNK * MP_ENTRY_SIZE];
    CHECK(fread(cat_bak, 1, sizeof(cat_bak), src) == sizeof(cat_bak));
    fclose(src);

    FILE *bak = fopen("./mp/catalog.bak", "wb");
    CHECK(bak != NULL);
    CHECK(fwrite(cat_bak, 1, sizeof(cat_bak), bak) == sizeof(cat_bak));
    fclose(bak);
    CHECK(unlink("./mp/catalog.bin") == 0);

    /* 重新初始化存储应返回 0 并从 catalog.bak 无损恢复书目 */
    CHECK(mp_store_init() == 0);
    CHECK_EQ_U(mp_store_count_done(), 1u);

    /* 场景 B：catalog.bin 存在但内容损坏（同时存在上一版 catalog.bak）
     * -> 留证为 catalog.bad，报损(-1)但数据从 catalog.bak 抢救回来 */
    FILE *bak2 = fopen("./mp/catalog.bak", "wb");
    CHECK(bak2 != NULL);
    CHECK(fwrite(cat_bak, 1, sizeof(cat_bak), bak2) == sizeof(cat_bak));
    fclose(bak2);
    FILE *bad = fopen("./mp/catalog.bin", "wb");
    CHECK(bad != NULL);
    CHECK(fwrite("CORRUPT", 1, 7, bad) == 7);
    fclose(bad);

    CHECK(mp_store_init() != 0);
    struct stat st_bad;
    CHECK(stat("./mp/catalog.bad", &st_bad) == 0);
    CHECK_EQ_U(mp_store_count_done(), 1u);

    /* 覆盖同一槽内容增加新章节（但保留旧 toc0.bin）-> mp_toc_ensure 必须自动失效重建 */
    const char *v2 =
        "\xB5\xDA\xD2\xBB\xD5\xC2 \xBF\xAA\xC6\xAA\nBody one.\n"
        "\xB5\xDA\xB6\xFE\xD5\xC2 \xD0\xF8\xC6\xAA\nBody two.\n";
    FILE *bf = fopen("./mp/b0.bin", "wb");
    CHECK(bf != NULL);
    CHECK(fwrite(v2, 1, strlen(v2), bf) == strlen(v2));
    fclose(bf);
    CHECK(mp_store_begin(0, "RecoverTest", (uint32_t)strlen(v2), MP_ENC_GB2312) == 0);
    CHECK(mp_store_write(0, 0, v2, strlen(v2)) == 0);
    CHECK(mp_store_finish(0) == 0);

    cnt = 0;
    CHECK(mp_toc_ensure(0, (uint32_t)strlen(v2), MP_ENC_GB2312, &cnt) == 0);
    CHECK_EQ_U(cnt, 2u);

    /* 场景 C：旧版固定 8 槽(264 B) catalog.bin + 旧版 64 B progress.bin 无损迁移与扩容 */
    uint8_t leg_cat[8 + 8 * MP_ENTRY_SIZE];
    memset(leg_cat, 0xFF, sizeof(leg_cat));
    leg_cat[0] = 'B'; leg_cat[1] = 'O'; leg_cat[2] = 'O'; leg_cat[3] = 'K';
    leg_cat[4] = 1; leg_cat[5] = 0; leg_cat[6] = 0; leg_cat[7] = 0;
    const char *leg_body = "Legacy book body content for seamless migration test.\n";
    uint32_t leg_len = (uint32_t)strlen(leg_body);
    for (uint8_t s = 0; s < 8u; s++) {
        uint8_t *e = &leg_cat[8u + (size_t)s * MP_ENTRY_SIZE];
        memset(e, 0, MP_ENTRY_SIZE);
        e[0] = MP_FLAG_DONE;
        snprintf((char *)&e[1], MP_TITLE_MAX + 1, "Legacy_%u", (unsigned)s);
        e[0x19] = (uint8_t)(leg_len & 0xFFu);
        e[0x1A] = (uint8_t)((leg_len >> 8) & 0xFFu);
        e[0x1D] = MP_ENC_ASCII;
        char bp[32];
        snprintf(bp, sizeof(bp), "./mp/b%u.bin", (unsigned)s);
        FILE *f = fopen(bp, "wb");
        CHECK(f != NULL);
        CHECK(fwrite(leg_body, 1, leg_len, f) == leg_len);
        fclose(f);
    }
    FILE *cf = fopen("./mp/catalog.bin", "wb");
    CHECK(cf != NULL);
    CHECK(fwrite(leg_cat, 1, sizeof(leg_cat), cf) == sizeof(leg_cat));
    fclose(cf);
    (void)unlink("./mp/catalog.bak");

    /* 旧版 64 B progress.bin：第 6 字节恒为 0，给 slot 7 写入旧进度 33 */
    uint8_t leg_prog[8 * 8];
    memset(leg_prog, 0, sizeof(leg_prog));
    uint8_t *p7 = &leg_prog[7 * 8];
    p7[0] = 0x5A;
    p7[1] = 7u;
    p7[2] = 33u;
    p7[6] = 0u;
    uint8_t x = 0;
    for (int i = 0; i < 7; i++) {
        x ^= p7[i];
    }
    p7[7] = x;
    FILE *pf = fopen("./mp/progress.bin", "wb");
    CHECK(pf != NULL);
    CHECK(fwrite(leg_prog, 1, sizeof(leg_prog), pf) == sizeof(leg_prog));
    fclose(pf);

    /* 重新初始化并验证老书 8 本与老进度无损读出，再上传第 9、10 本自动扩容至 16 槽 */
    CHECK(mp_store_init() == 0);
    CHECK_EQ_U(mp_store_count_done(), 8u);
    CHECK_EQ_U(mp_store_slot_capacity(), 8u);
    uint32_t pos7 = 0;
    CHECK(mp_progress_load(7u, &pos7));
    CHECK_EQ_U(pos7, 33u);

    uint16_t s8 = ble_upload_book("NewBook_8", (const uint8_t *)leg_body,
                                  leg_len, MP_ENC_ASCII);
    uint16_t s9 = ble_upload_book("NewBook_9", (const uint8_t *)leg_body,
                                  leg_len, MP_ENC_ASCII);
    CHECK_EQ_U(s8, 8u);
    CHECK_EQ_U(s9, 9u);
    CHECK_EQ_U(mp_store_count_done(), 10u);
    CHECK_EQ_U(mp_store_slot_capacity(), 16u);
    CHECK(mp_progress_save(9u, 27u) == 0);
    uint32_t pos9 = 0;
    CHECK(mp_progress_load(7u, &pos7));
    CHECK_EQ_U(pos7, 33u);
    CHECK(mp_progress_load(9u, &pos9));
    CHECK_EQ_U(pos9, 27u);

    /* 场景 D：16-bit 高槽号(>= 255，如 slot 260) 全链路验证：
     * 0x14 饱和计数 + 真实 16-bit、0x15 扩展 16/12/8 B 回包与 4 B 失败包、
     * 0x20 打开高槽、progress.bin idx_hi!=0 持久化、0x13 删除高槽与 0x1A 收缩。 */
    uint8_t big_cat[8 + 264 * MP_ENTRY_SIZE];
    memset(big_cat, 0xFF, sizeof(big_cat));
    big_cat[0] = 'B'; big_cat[1] = 'O'; big_cat[2] = 'O'; big_cat[3] = 'K';
    big_cat[4] = 1; big_cat[5] = 0; big_cat[6] = 0; big_cat[7] = 0;
    /* 填满 261 个槽(0..260)，重点测槽 255 与槽 260 */
    for (uint16_t s = 0; s <= 260u; s++) {
        uint8_t *e = &big_cat[8u + (size_t)s * MP_ENTRY_SIZE];
        memset(e, 0, MP_ENTRY_SIZE);
        e[0] = MP_FLAG_DONE;
        snprintf((char *)&e[1], MP_TITLE_MAX + 1, "HighSlot_%03u", (unsigned)s);
        e[0x19] = (uint8_t)(leg_len & 0xFFu);
        e[0x1A] = (uint8_t)((leg_len >> 8) & 0xFFu);
        e[0x1D] = MP_ENC_ASCII;
    }
    FILE *bf260 = fopen("./mp/b260.bin", "wb");
    CHECK(bf260 != NULL);
    CHECK(fwrite(leg_body, 1, leg_len, bf260) == leg_len);
    fclose(bf260);
    FILE *bcf = fopen("./mp/catalog.bin", "wb");
    CHECK(bcf != NULL);
    CHECK(fwrite(big_cat, 1, sizeof(big_cat), bcf) == sizeof(big_cat));
    fclose(bcf);
    (void)unlink("./mp/catalog.bak");

    CHECK(mp_store_init() == 0);
    CHECK_EQ_U(mp_store_count_done(), 261u);
    CHECK_EQ_U(mp_store_slot_capacity(), 264u);

    /* 0x14：旧字节饱和到 255，16-bit 扩展给出真实总数 261 与槽容量 264 */
    sim_hal_ble_clear_tx();
    ble_send(MP_OP_BOOK_COUNT, NULL, 0);
    CHECK_EQ_U(sim_hal_ble_tx_count(), 1u);
    const sim_ble_pkt_t *p14 = sim_hal_ble_tx_at(0);
    CHECK_EQ_U(p14->len, 6u);
    CHECK_EQ_U(p14->data[1], 255u);
    CHECK_EQ_U((uint16_t)(p14->data[2] | (p14->data[3] << 8)), 261u);
    CHECK_EQ_U((uint16_t)(p14->data[4] | (p14->data[5] << 8)), 264u);

    /* 0x15：查询 slot 260 (0x0104) 应返回 16/12/8 B 三包且末字节均为 idx_hi=0x01 */
    uint8_t q260[2] = {0x04u, 0x01u};
    sim_hal_ble_clear_tx();
    ble_send(MP_OP_BOOK_GET, q260, 2);
    CHECK_EQ_U(sim_hal_ble_tx_count(), 3u);
    CHECK_EQ_U(sim_hal_ble_tx_at(0)->len, 16u);
    CHECK_EQ_U(sim_hal_ble_tx_at(0)->data[1], 0x04u);
    CHECK_EQ_U(sim_hal_ble_tx_at(0)->data[15], 0x01u);
    CHECK(memcmp(sim_hal_ble_tx_at(0)->data + 3, "HighSlot_260", 12) == 0);
    CHECK_EQ_U(sim_hal_ble_tx_at(1)->len, 12u);
    CHECK_EQ_U(sim_hal_ble_tx_at(1)->data[11], 0x01u);
    CHECK_EQ_U(sim_hal_ble_tx_at(2)->len, 8u);
    CHECK_EQ_U(sim_hal_ble_tx_at(2)->data[7], 0x01u);

    /* 0x15：查询空槽 262 (0x0106) 应返回 4 B 失败包 [0x15, 0x06, 0xFF, 0x01] */
    uint8_t q262[2] = {0x06u, 0x01u};
    sim_hal_ble_clear_tx();
    ble_send(MP_OP_BOOK_GET, q262, 2);
    CHECK_EQ_U(sim_hal_ble_tx_count(), 1u);
    CHECK_EQ_U(sim_hal_ble_tx_at(0)->len, 4u);
    CHECK_EQ_U(sim_hal_ble_tx_at(0)->data[1], 0x06u);
    CHECK_EQ_U(sim_hal_ble_tx_at(0)->data[2], MP_ST_FAIL);
    CHECK_EQ_U(sim_hal_ble_tx_at(0)->data[3], 0x01u);

    /* progress.bin：slot 260 (idx_hi=1) 与 slot 4 (idx_hi=0) 独立存储互不串位 */
    CHECK(mp_progress_save(4u, 11u) == 0);
    CHECK(mp_progress_save(260u, 29u) == 0);
    uint32_t pos4 = 0, pos260 = 0;
    CHECK(mp_progress_load(4u, &pos4));
    CHECK(mp_progress_load(260u, &pos260));
    CHECK_EQ_U(pos4, 11u);
    CHECK_EQ_U(pos260, 29u);

    /* 0x20 远程打开高槽 260，随后 0x13 用 2 B 槽号删除 260 */
    sim_hal_ble_clear_tx();
    ble_send(MP_OP_EBOOK_OPEN, q260, 2);
    CHECK_EQ_U(sim_hal_ble_tx_at(0)->data[1], MP_ST_OK);
    CHECK_EQ_U(mp_app_state.screen, MP_SCREEN_READ);
    CHECK_EQ_U(mp_app_state.book_idx, 260u);

    sim_hal_ble_clear_tx();
    ble_send(MP_OP_BOOK_DELETE, q260, 2);
    CHECK_EQ_U(sim_hal_ble_tx_at(0)->data[1], MP_ST_OK);
    CHECK_EQ_U(mp_store_count_done(), 260u);
    CHECK_EQ_U(mp_app_state.book_open, 0u);
    sim_hal_shutdown();
}

/* ------------------------------------------------------------------ *
 * T15：锁屏深睡插电唤醒 + RTC_CNTL_PAD_HOLD 解锁 + 彻底掉电 clock.bin 兜底
 * ------------------------------------------------------------------ */
static void t15_step1_lock_sleep_save_clock(const char *rtc_file)
{
    sim_hal_init();
    mp_app_boot();
    sim_hal_ble_connect(true, 247);

    /* 对时到 2026-06-15 14:30:00（周一，UTC=1781533800） */
    mp_time_t t = {
        .unix = 1781533800u,
        .year = 2026,
        .month = 6,
        .day = 15,
        .week = 1,
        .hour = 14,
        .min = 30,
        .sec = 0,
    };
    sim_hal_ble_inject_time(&t);
    advance_ticks(100, 100);
    sim_hal_ble_connect(false, 247);

    /* 校验已校准时间立即落盘 /sdcard/mp/clock.bin */
    uint32_t disk_ux = 0;
    CHECK(mp_clock_load(&disk_ux));
    CHECK(disk_ux >= 1781533800u && disk_ux <= 1781533810u);

    /* 关蓝牙并长按 OK 锁屏，等待进入深睡 */
    mp_settings.ble_enabled = 0;
    mp_ble_stop();
    press_long(KEY_OK);
    CHECK_EQ_U(mp_app_state.screen, MP_SCREEN_LOCK);

    int trap = setjmp(*sim_hal_trap_buf());
    if (trap == 0) {
        sim_hal_arm_trap(true);
        advance_ticks(MP_LOCK_SLEEP_GRACE_MS + 2000u, 500u);
    }
    sim_hal_arm_trap(false);
    CHECK_EQ_U(trap, SIM_TRAP_DEEP_SLEEP);

    const sim_sleep_audit_t *sa = sim_hal_sleep_audit();
    CHECK(sa->rtc_periph_kept);
    CHECK(!sa->deep_sleep_hold);
    CHECK_EQ_U(sa->rtc_pad_held_mask, 0ULL);
    CHECK_EQ_U(sa->ext1_mask,
               (1ULL << BOARD_KEY_OK_GPIO) | (1ULL << BOARD_CHARGE_DET_GPIO));
    CHECK(sim_hal_rtc_save(rtc_file) == 0);
    sim_hal_shutdown();
}

static void t15_step2_wake_by_charger_from_lock(const char *rtc_file)
{
    /* 锁屏深睡 1 小时 15 分钟后插入充电线：无需按确认键，直接唤醒并弹充电弹窗。 */
    sim_hal_init();
    CHECK(sim_hal_rtc_load(rtc_file) == 0);
    sim_hal_sleep_advance_ms((1ULL * 3600ULL + 15ULL * 60ULL) * 1000ULL);
    sim_hal_set_battery(65, 3890, true, true);
    /* 故意注入遗留 RTC_CNTL_PAD_HOLD，验证 early_wake_check 先清 hold 再读 IO。 */
    sim_hal_force_rtc_pad_hold((1ULL << BOARD_KEY_OK_GPIO) |
                               (1ULL << BOARD_CHARGE_DET_GPIO));
    sim_hal_set_wakeup(1u << ESP_SLEEP_WAKEUP_EXT1, 1ULL << BOARD_CHARGE_DET_GPIO);

    mp_app_early_wake_check();
    CHECK_EQ_U(sim_hal_rtc_pad_held_mask(), 0ULL);
    mp_app_boot();

    CHECK_EQ_U(mp_app_state.screen, MP_SCREEN_CLOCK);
    CHECK_EQ_U(mp_app_state.charge_popup, 1u);
    CHECK_EQ_U(mp_app_state.month, 6u);
    CHECK_EQ_U(mp_app_state.day, 15u);
    CHECK_EQ_U(mp_app_state.hour, 15u);
    CHECK(mp_app_state.min == 45u);

    /* 在持续充电中（GPIO5==0）锁屏进深睡：EXT1 掩码必须排除 GPIO5，防秒醒循环！ */
    advance_ticks(MP_CHARGE_POPUP_MS + 200u, 200u);
    CHECK_EQ_U(mp_app_state.charge_popup, 0u);
    mp_settings.ble_enabled = 0;
    mp_ble_stop();
    press_long(KEY_OK);
    CHECK_EQ_U(mp_app_state.screen, MP_SCREEN_LOCK);

    int trap = setjmp(*sim_hal_trap_buf());
    if (trap == 0) {
        sim_hal_arm_trap(true);
        advance_ticks(MP_LOCK_SLEEP_GRACE_MS + 2000u, 500u);
    }
    sim_hal_arm_trap(false);
    CHECK_EQ_U(trap, SIM_TRAP_DEEP_SLEEP);
    const sim_sleep_audit_t *sa = sim_hal_sleep_audit();
    CHECK_EQ_U(sa->ext1_mask, 1ULL << BOARD_KEY_OK_GPIO);
    sim_hal_shutdown();
}

static void t15_step3_recover_from_forced_pad_hold(const char *rtc_file)
{
    /* 专门还原真机故障：若外部/旧镜像导致 GPIO8 留有 RTC_CNTL_PAD_HOLD，
     * 唤醒后必须先 rtc_gpio_hold_dis(GPIO8) 才能读出低电平解锁，绝不陷入深睡死循环。 */
    sim_hal_init();
    CHECK(sim_hal_rtc_load(rtc_file) == 0);
    sim_hal_sleep_advance_ms(600000ULL);
    sim_hal_force_rtc_pad_hold(1ULL << BOARD_KEY_OK_GPIO);
    sim_hal_set_wakeup(1u << ESP_SLEEP_WAKEUP_EXT1, 1ULL << BOARD_KEY_OK_GPIO);
    sim_hal_set_ok_hold_ms(3000u); /* 持续按住无需松手，满 MP_WAKE_HOLD_MS 即刻唤醒 */

    mp_app_early_wake_check();
    CHECK_EQ_U(sim_hal_rtc_pad_held_mask(), 0ULL);
    mp_app_boot();
    CHECK_EQ_U(mp_app_state.screen, MP_SCREEN_CLOCK);
    CHECK_EQ_U(mp_app_state.hour, 14u);
    CHECK_EQ_U(mp_app_state.min, 40u);
    sim_hal_shutdown();
}

static void t15_step4_por_restore_clock_from_sd(const char *rtc_file)
{
    (void)rtc_file;
    /* 模拟彻底断电 POR（RTC_DATA_ATTR 清零 + 硬件 RTC 墙钟归零）：
     * 冷启动应从 /sdcard/mp/clock.bin 恢复上次保存的时间，绝不惨回 2026-01-01 00:00！ */
    sim_hal_init();
    sim_hal_clear_rtc_mem();
    mp_app_boot();

    CHECK_EQ_U(mp_app_state.screen, MP_SCREEN_CLOCK);
    CHECK_EQ_U(mp_app_state.month, 6u);
    CHECK_EQ_U(mp_app_state.day, 15u);
    CHECK_EQ_U(mp_app_state.hour, 14u);
    CHECK_EQ_U(mp_app_state.min, 30u);

    /* 同时验证损坏 clock.bin 时可从 clock.bak 无损降级恢复。 */
    uint32_t snap_ux = 1781620200u; /* 2026-06-16 14:30:00 */
    CHECK(mp_clock_save(snap_ux) == 0);
    /* 再次写入让 clock.bak 保存 snap_ux，再故意破坏 clock.bin。 */
    CHECK(mp_clock_save(snap_ux + 60u) == 0);
    FILE *bad = fopen("./mp/clock.bin", "wb");
    CHECK(bad != NULL);
    CHECK(fwrite("BROKEN!", 1, 7, bad) == 7);
    fclose(bad);
    uint32_t loaded = 0;
    CHECK(mp_clock_load(&loaded));
    CHECK_EQ_U(loaded, snap_ux);
    sim_hal_shutdown();
}

static void t15_step5_uncalibrated_never_saves_default_clock(const char *rtc_file)
{
    (void)rtc_file;
    /* 全新沙盒无 clock.bin、未对时：锁屏不应把默认 2026-01-01 写进 clock.bin。 */
    (void)unlink("./mp/clock.bin");
    (void)unlink("./mp/clock.bak");
    sim_hal_init();
    sim_hal_clear_rtc_mem();
    mp_app_boot();
    CHECK_EQ_U(mp_app_state.month, 1u);
    CHECK_EQ_U(mp_app_state.day, 1u);
    press_long(KEY_OK);
    CHECK_EQ_U(mp_app_state.screen, MP_SCREEN_LOCK);
    uint32_t dummy = 0;
    CHECK(!mp_clock_load(&dummy));
    sim_hal_shutdown();
}

static void test_t15_deep_sleep_charge_hold_and_por_clock(void)
{
    const char *rtc_file = "./t15_rtc.bin";
    CHECK(run_child_step(t15_step1_lock_sleep_save_clock, rtc_file) == 0);
    CHECK(run_child_step(t15_step2_wake_by_charger_from_lock, rtc_file) == 0);
    CHECK(run_child_step(t15_step3_recover_from_forced_pad_hold, rtc_file) == 0);
    CHECK(run_child_step(t15_step4_por_restore_clock_from_sd, rtc_file) == 0);
    CHECK(run_child_step(t15_step5_uncalibrated_never_saves_default_clock, rtc_file) == 0);
}

/* ------------------------------------------------------------------ *
 * T16：真实 power_filter.c 充放电防抖、插拔 ±20% IR 补偿与跨深睡压测
 * ------------------------------------------------------------------ */
static void test_t16_power_filter_debounce_and_ir_comp(void)
{
    power_filter_rtc_t rtc;
    power_filter_run_t run;
    memset(&rtc, 0, sizeof(rtc));
    power_filter_boot_init(&rtc, &run);

    uint64_t t = 1000u;

    /* 1) 冷启动未充电：3800 mV -> (3800+25-3500)*100/600 = 54% */
    uint8_t p = power_filter_update(&rtc, &run, 3800, false, t);
    CHECK_EQ_U(p, 54u);
    CHECK_EQ_U(rtc.last_idle_mv, 3800u);

    /* 2) 稳态放电毛刺与单向防抖：负载释放回弹到 3860 mV 绝不反涨 */
    t += 60000u;
    p = power_filter_update(&rtc, &run, 3860, false, t);
    CHECK_EQ_U(p, 54u);

    /* 突然压降到 3720 mV（裸值 40%）：受 IIR + 每周期最多降 2% 限制，稳步下探 */
    t += 60000u;
    p = power_filter_update(&rtc, &run, 3720, false, t);
    CHECK(p >= 52u && p <= 54u);

    /* 恢复到 3800 mV 稳态静置，准备测插电瞬间压升 +120 mV（原本裸值跳 +20%） */
    for (int i = 0; i < 6; i++) {
        t += 60000u;
        p = power_filter_update(&rtc, &run, 3800, false, t);
    }
    uint8_t before_plug = p;
    CHECK(before_plug >= 52u && before_plug <= 54u);

    /* 3) 插电瞬间电压飙升 +120 mV（3800 -> 3920 mV，裸算会立刻飙到 74%）:
     *    必须实现 0% 跳变，且自适应标定 IR 偏置约为 120 mV！ */
    t += 1000u;
    p = power_filter_update(&rtc, &run, 3920, true, t);
    CHECK_EQ_U(p, before_plug);
    CHECK(rtc.chg_offset_mv >= 110u && rtc.chg_offset_mv <= 130u);

    /* 15 s 极化过渡窗内即便电压再波动到 3940 mV 也不跳变 */
    t += 5000u;
    p = power_filter_update(&rtc, &run, 3940, true, t);
    CHECK_EQ_U(p, before_plug);

    /* 4) 稳态持续充电：单调平滑上升，每次步进不超过 +2% */
    uint8_t prev = p;
    for (int step = 1; step <= 18; step++) {
        t += 60000u;
        int mv = 3940 + step * 8; /* 从 3948 缓慢充到 4084 mV */
        p = power_filter_update(&rtc, &run, mv, true, t);
        CHECK(p >= prev);
        CHECK((uint8_t)(p - prev) <= POWER_DISP_STEP_UP_MAX);
        prev = p;
    }
    CHECK(p > before_plug);

    /* 5) CV 恒压区线性衰减校验：4080 mV 满偏置、4130 mV 半偏置、4180 mV 归零 */
    CHECK_EQ_U(power_filter_effective_chg_offset(4080, 120), 120u);
    CHECK_EQ_U(power_filter_effective_chg_offset(4130, 120), 60u);
    CHECK_EQ_U(power_filter_effective_chg_offset(4180, 120), 0u);

    /* 6) 充电至中途拔掉充电线：端电压瞬间跌落 -120 mV（如 4020 -> 3900 mV），
     *    必须 0% 蒸发，且 15 s 过渡窗内反复插拔 5 次零漂移、不污染 IR 偏置！ */
    uint8_t before_unplug = p;
    uint16_t saved_off = rtc.chg_offset_mv;
    t += 2000u;
    p = power_filter_update(&rtc, &run, 3900, false, t);
    CHECK_EQ_U(p, before_unplug);

    for (int k = 0; k < 5; k++) {
        t += 1000u;
        p = power_filter_update(&rtc, &run, 4020, true, t);
        CHECK_EQ_U(p, before_unplug);
        t += 1000u;
        p = power_filter_update(&rtc, &run, 3900, false, t);
        CHECK_EQ_U(p, before_unplug);
    }
    CHECK_EQ_U(rtc.chg_offset_mv, saved_off);

    /* 7) 继续充入 CV 满电区（4180 mV）：最终能平滑充满到 100% */
    t += 30000u;
    (void)power_filter_update(&rtc, &run, 4080, true, t);
    for (int i = 0; i < 30; i++) {
        t += 60000u;
        p = power_filter_update(&rtc, &run, 4180, true, t);
    }
    CHECK_EQ_U(p, 100u);

    /* 8) 慢软启动充电 IC 场景：插电瞬间仅抬升 8 mV，60 s 后才满流跳升 +140 mV，
     *    首周期补标定应吞掉满流压升而不虚涨 20%！ */
    memset(&rtc, 0, sizeof(rtc));
    power_filter_boot_init(&rtc, &run);
    t = 1000u;
    CHECK_EQ_U(power_filter_update(&rtc, &run, 3800, false, t), 54u);
    t += 2000u;
    CHECK_EQ_U(power_filter_update(&rtc, &run, 3808, true, t), 54u);
    t += 60000u;
    p = power_filter_update(&rtc, &run, 3948, true, t);
    CHECK(p <= 56u); /* 绝不会虚涨到 74% */

    /* 9) 跨深睡验证：
     *    9a) 深睡前未充电 54%，深睡中插电充满至 4190 mV 唤醒 -> 直接同步 100%；
     *    9b) 深睡前未充电 60%，短睡小幅回弹至 3850 mV(62%) -> 不反涨仍 60%；
     *    9c) 深睡前未充电 60%，长睡真实耗电至 3740 mV(44%) -> 唤醒立即同步 44%。 */
    memset(&rtc, 0, sizeof(rtc));
    power_filter_boot_init(&rtc, &run);
    CHECK_EQ_U(power_filter_update(&rtc, &run, 3800, false, 1000u), 54u);
    power_filter_boot_init(&rtc, &run); /* 模拟深睡唤醒重启 */
    CHECK_EQ_U(power_filter_update(&rtc, &run, 4190, true, 100u), 100u);

    memset(&rtc, 0, sizeof(rtc));
    power_filter_boot_init(&rtc, &run);
    CHECK_EQ_U(power_filter_update(&rtc, &run, 3835, false, 1000u), 60u);
    power_filter_boot_init(&rtc, &run);
    CHECK_EQ_U(power_filter_update(&rtc, &run, 3850, false, 100u), 60u);
    power_filter_boot_init(&rtc, &run);
    CHECK_EQ_U(power_filter_update(&rtc, &run, 3740, false, 100u), 44u);

    /* 10) 危急低电安全快车道：真实电压跌至 3480 mV (raw=0% < 3%)，
     *     第 1 次确认不误关，连续第 2 次确认立即绕过 2% 步长限幅下探至 0%！ */
    memset(&rtc, 0, sizeof(rtc));
    power_filter_boot_init(&rtc, &run);
    t = 1000u;
    CHECK_EQ_U(power_filter_update(&rtc, &run, 3600, false, t), 20u);
    t += 60000u;
    p = power_filter_update(&rtc, &run, 3480, false, t);
    CHECK(p >= 18u); /* 第 1 次毛刺不立刻暴跌关机 */
    t += 60000u;
    p = power_filter_update(&rtc, &run, 3480, false, t);
    CHECK_EQ_U(p, 0u); /* 连续第 2 次确认立刻走安全快车道保护电池 */
}

/* ------------------------------------------------------------------ *
 * T17：屏幕周期页内合并 GC + 交互微调页暂缓自动 GC + 关闭空闲突袭全刷
 * ------------------------------------------------------------------ */
static void test_t17_display_gc_policy_and_interactive_defer(void)
{
    sim_hal_init();
    mp_app_boot();

    /* 1) 验证开机配置：默认每 10 次局刷合并一次全刷，且关闭空闲原地全刷 (idle_ms == 0) */
    uint8_t gc_p = 0;
    uint16_t gc_idle = 1;
    sim_hal_display_get_gc(&gc_p, &gc_idle);
    CHECK_EQ_U(gc_p, 10u);
    CHECK_EQ_U(gc_idle, 0u);

    /* 上传一本足够长的书（> 15 页），进入阅读页 */
    sim_hal_ble_connect(true, 247);
    char body[8192];
    size_t pos = 0;
    for (int i = 0; i < 120; i++) {
        int n = snprintf(body + pos, sizeof(body) - pos,
                         "Line %03d verifying periodic in-page GC merge on BOE panel.\n", i);
        CHECK(n > 0 && (size_t)n < sizeof(body) - pos);
        pos += (size_t)n;
    }
    (void)ble_upload_book("GC_Test", (const uint8_t *)body, (uint32_t)pos, MP_ENC_ASCII);
    sim_hal_ble_connect(false, 247);

    press_click(KEY_NEXT); /* CLOCK -> SELECT */
    press_ok_single();     /* SELECT -> READ (首开书全刷或清基准) */
    CHECK_EQ_U(mp_app_state.screen, MP_SCREEN_READ);
    /* 先手动全刷建立干净的 0 计数基准 */
    display_request_full();
    press_click(KEY_NEXT);
    display_stats_t ds;
    display_get_stats(&ds);
    CHECK_EQ_U(ds.partial_since_full, 0u);
    uint32_t fulls_base = ds.fulls;

    /* 2) 阅读页连续翻 9 页应全部走局刷，翻第 10 页当次直接合并为全刷！ */
    for (int i = 1; i <= 9; i++) {
        press_click(KEY_NEXT);
        display_get_stats(&ds);
        CHECK_EQ_U(ds.fulls, fulls_base);
        CHECK_EQ_U(ds.partial_since_full, (uint32_t)i);
    }
    press_click(KEY_NEXT); /* 第 10 次翻页本身直接合并全刷 */
    display_get_stats(&ds);
    CHECK_EQ_U(ds.fulls, fulls_base + 1u);
    CHECK_EQ_U(ds.partial_since_full, 0u);
    CHECK(!ds.gc_pending);

    /* 3) 交互微调页暂缓自动 GC：进入设置页并连续滚动 15 次，绝不插队全刷！ */
    press_click(KEY_MENU); /* READ -> SELECT */
    press_click(KEY_MENU); /* SELECT -> CLOCK */
    press_click(KEY_PREV); /* CLOCK -> SETTINGS */
    CHECK_EQ_U(mp_app_state.screen, MP_SCREEN_SETTINGS);

    display_get_stats(&ds);
    uint32_t fulls_in_menu = ds.fulls;
    for (int i = 0; i < 15; i++) {
        press_click(KEY_NEXT);
    }
    display_get_stats(&ds);
    CHECK_EQ_U(ds.fulls, fulls_in_menu); /* 15 次连按全程跟手局刷，0 次插队全刷 */
    CHECK(ds.gc_pending);                /* 已挂起待办，等待退出微调页后合并清残影 */

    /* 4) 退出设置页回到时钟屏（常规页）：这一帧自动合并成一次全刷清光残影！ */
    press_click(KEY_MENU); /* SETTINGS -> CLOCK */
    CHECK_EQ_U(mp_app_state.screen, MP_SCREEN_CLOCK);
    display_get_stats(&ds);
    CHECK_EQ_U(ds.fulls, fulls_in_menu + 1u);
    CHECK_EQ_U(ds.partial_since_full, 0u);
    CHECK(!ds.gc_pending);

    /* 5) 快捷背光浮窗微调同样全程走局刷，确认关闭浮窗回常规页若已满阈值再合并清残影 */
    sim_hal_set_backlight_hw(true);
    press_long(KEY_FUNC);
    CHECK_EQ_U(mp_app_state.bl_popup, 1u);
    display_get_stats(&ds);
    uint32_t fulls_bl = ds.fulls;
    for (int i = 0; i < 12; i++) {
        press_click((i & 1) ? KEY_PREV : KEY_NEXT);
    }
    display_get_stats(&ds);
    CHECK_EQ_U(ds.fulls, fulls_bl);
    CHECK(ds.gc_pending);
    press_ok_single(); /* 确认关闭浮窗回到 CLOCK，立即顺手合并全刷清残影 */
    CHECK_EQ_U(mp_app_state.bl_popup, 0u);
    display_get_stats(&ds);
    CHECK_EQ_U(ds.fulls, fulls_bl + 1u);
    CHECK(!ds.gc_pending);

    sim_hal_shutdown();
}

/* ------------------------------------------------------------------ *
 * 测试运行器（每项在独立 fork 子进程与独立临时目录中执行）
 * ------------------------------------------------------------------ */
typedef struct {
    const char *id;
    const char *desc;
    void (*fn)(void);
} fw_test_t;

static const fw_test_t k_tests[] = {
    { "T01a", "启动初始化 / SHA-256 / 版本号日期单一真源 / E2S 设置", test_t01_boot_about_and_hw_adapt },
    { "T01b", "E2 无背光自适应 / SD 存储错误兜底与恢复",             test_t01b_e2_no_backlight_and_storage_err },
    { "T02a", "插电 5 秒竖版电池弹窗 + 顶栏小闪电 + 插拔复现",         test_t02_charge_popup_and_icon },
    { "T02b", "低电态插电 EXT1 唤醒直出充电弹窗",                      test_t02b_charge_ext1_wakeup_popup },
    { "T03",  "电子书 BLE 上传 / 阅读翻页 / 自动目录缓存跳章 / 删除",  test_t03_ebook_toc_read_lifecycle },
    { "T04",  "无上限动态扩容(20本/槽复用/压缩)与空间不足/异常防御",         test_t04_upload_guards_and_recovery },
    { "T05",  "锁屏图 BLE 上传 / 160B 回读 / y+6 逐像素映射",          test_t05_lock_image_upload_readback_and_mapping },
    { "T06",  "自动锁屏熄灭背光 / 解锁恢复背光 / 快捷浮窗 / 省电关蓝牙", test_t06_auto_lock_backlight_and_ble_power_save },
    { "T07",  "锁屏态 BLE 安全白名单隔离（禁删书/禁改锁图/禁 OTA）",   test_t07_lock_ble_whitelist_security },
    { "T08",  "跨深睡双进程真重启 / 误触过滤 / 跨月走时精准保持",      test_t08_deep_sleep_rtc_and_miswake },
    { "T09",  "闰年 02-29 日历进位 + 手动时间设置 5 步向导",           test_t09_leap_year_and_time_set },
    { "T10",  "低电 <3% 全刷提示屏 + 进充电唤醒深睡",                  test_t10_low_battery_shutdown },
    { "T11",  "原生 BLE 原地 OTA 全链路（真包校验 + IRAM 覆写审计）",  test_t11_native_ble_ota_full_real_image },
    { "T12",  "原生 BLE OTA 9 项安全拦截矩阵（防低电/篡改/误刷原厂）", test_t12_ota_security_rejections },
    { "T13a", "恢复原厂固件（武装门 + 双扇区 otadata 安全切换）",      test_t13_restore_stock_flow_and_safety },
    { "T13b", "原厂槽镜像损坏时拒绝切换且零写入 otadata",              test_t13b_restore_stock_rejects_corrupt_slot },
    { "T14",  "catalog.bak 掉电恢复 + TOC 缓存 CRC 失效自动重建",      test_t14_store_power_loss_and_toc_invalidation },
    { "T15",  "锁屏深睡插电唤醒 / RTC PAD HOLD 释放 / 掉电时间快照恢复", test_t15_deep_sleep_charge_hold_and_por_clock },
    { "T16",  "真实 power_filter 电量防抖 / 插拔 ±20% IR 补偿 / 快车道", test_t16_power_filter_debounce_and_ir_comp },
    { "T17",  "屏幕周期页内合并 GC / 微调页暂缓全刷 / 关闭空闲突袭",     test_t17_display_gc_policy_and_interactive_defer },
};

int main(int argc, char **argv)
{
    const char *filter = NULL;
    if (argc >= 2 && strcmp(argv[1], "--only") == 0 && argc >= 3) {
        filter = argv[2];
    }

    /* 主进程加载只读字库资产（HZK16 + gb2312_map），fork 后各子进程写时复制共享。 */
    sim_text_init(getenv("MP_SIM_ASSETS"));

    /* 保存原始工作目录，以便在需要时解析相对路径的固件包。 */
    char orig_cwd[1024];
    if (!getcwd(orig_cwd, sizeof(orig_cwd))) {
        perror("getcwd");
        return 2;
    }
    if (!getenv("MP_SIM_OTA_BIN")) {
        char default_bin[1200];
        int n = snprintf(default_bin, sizeof(default_bin),
                         "%s/build/miaopaper_e2_e2s.bin", orig_cwd);
        if (n > 0 && (size_t)n < sizeof(default_bin) && access(default_bin, R_OK) == 0) {
            setenv("MP_SIM_OTA_BIN", default_bin, 1);
        } else {
            n = snprintf(default_bin, sizeof(default_bin),
                         "%s/../build/miaopaper_e2_e2s.bin", orig_cwd);
            if (n > 0 && (size_t)n < sizeof(default_bin) && access(default_bin, R_OK) == 0) {
                setenv("MP_SIM_OTA_BIN", default_bin, 1);
            }
        }
    }

    size_t total = sizeof(k_tests) / sizeof(k_tests[0]);
    size_t ran = 0;
    size_t failed = 0;

    printf("=== E2/E2S 本机固件全链路回归测试 ===\n");
    for (size_t i = 0; i < total; i++) {
        if (filter && strcmp(filter, k_tests[i].id) != 0) {
            continue;
        }
        ran++;
        char tmpl[] = "/tmp/mp_fw_reg_XXXXXX";
        char *sandbox = mkdtemp(tmpl);
        if (!sandbox) {
            perror("mkdtemp");
            return 2;
        }

        fflush(stdout);
        fflush(stderr);
        pid_t pid = fork();
        if (pid < 0) {
            perror("fork");
            rm_rf_dir(sandbox);
            return 2;
        }
        if (pid == 0) {
            if (chdir(sandbox) != 0) {
                perror("chdir");
                _exit(2);
            }
            k_tests[i].fn();
            _exit(0);
        }

        int status = 0;
        if (waitpid(pid, &status, 0) != pid) {
            perror("waitpid");
            rm_rf_dir(sandbox);
            return 2;
        }
        rm_rf_dir(sandbox);

        if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
            printf("PASS %-5s %s\n", k_tests[i].id, k_tests[i].desc);
        } else if (WIFSIGNALED(status)) {
            printf("FAIL %-5s %s (signal %d)\n",
                   k_tests[i].id, k_tests[i].desc, WTERMSIG(status));
            failed++;
        } else {
            printf("FAIL %-5s %s (exit %d)\n",
                   k_tests[i].id, k_tests[i].desc, WEXITSTATUS(status));
            failed++;
        }
    }

    printf("fw_regress: %zu cases, %zu failure(s)\n", ran, failed);
    return failed == 0 ? 0 : 1;
}
