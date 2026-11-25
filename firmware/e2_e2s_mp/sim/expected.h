#pragma once
/* expected.h — 27 帧的 32 位像素哈希（sim --check 的唯一真源）。
 *
 * 哈希 = rc_crc32_ieee(framebuffer, MP_FB_BYTES)，在 mp_ui_render() 之后、
 * 任何 PGM 转换之前计算，包含每行 x=250..255 的填充位。任何绘制变化都必须
 * 重新生成：
 *
 *   ./build/sim_mp --hash
 *
 * 生成前先确认 sim_ui/sim_hints 都是 real（占位渲染器不会得到这些值）。
 */
#include <stdint.h>
#include <string.h>

typedef struct {
    const char *name;
    uint32_t crc;
} sim_expected_t;

static const sim_expected_t k_expected_frames[] = {
    { "clock",           0x19CD15C9u },
    { "select",          0x0864484Bu },
    { "read_cn",         0x8810CB34u },
    { "read_en",         0x96A0C84Au },
    { "read_end",        0x20107F3Cu },
    { "settings",        0xDDB32778u },
    { "settings_off",    0x19662E3Au },
    { "settings_scroll", 0x81ED22FDu },
    { "settings_e2",     0x07D79DF8u },
    { "backlight",       0x5C4811E0u },
    { "backlight_popup", 0xC8B7D696u },
    { "book_info",       0x6FEDF09Bu },
    { "jump",            0xF41BBB82u },
    { "time_set",        0x3851BBD2u },
    { "time_done",       0x84BC14F3u },
    { "delete",          0x15AC5928u },
    { "about",           0x9658A968u },
    { "about_e2",        0x4D4DF7A0u },
    { "lock_img",        0x05B429D0u },
    { "lock_stats",      0x3488E5C5u },
    { "lock_none",       0xF7A7DD72u },
    { "no_lock_img",     0xCACB811Fu },
    { "empty",           0x4A15D43Du },
    { "error",           0x65B60842u },
    { "low_batt",        0xF69FC842u },
    { "restore",         0x22DC895Cu },
    { "restore_ok",      0xE8FA4735u },
};

#define SIM_EXPECTED_COUNT \
    ((int)(sizeof(k_expected_frames) / sizeof(k_expected_frames[0])))

static inline uint32_t sim_expected_crc(const char *name)
{
    for (int i = 0; i < SIM_EXPECTED_COUNT; i++) {
        if (strcmp(k_expected_frames[i].name, name) == 0) {
            return k_expected_frames[i].crc;
        }
    }
    return 0;
}
