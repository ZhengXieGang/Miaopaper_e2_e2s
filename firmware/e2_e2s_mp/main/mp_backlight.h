#pragma once
/* mp_backlight.h — AWINIC AW2023 三通道 LED 驱动（背光仅用 LED0），支持 E2/E2S 自适应。
 *
 * 硬件规格（e2s_re/docs/BACKLIGHT.md / docs/E2_VS_E2S_FIRMWARE_DIFF.md）：
 *   - 7 位 I2C 地址：0x45，SDA = GPIO35，SCL = GPIO18，400 kHz 内部上拉；
 *   - 4 档亮度：level 0..3 对应 PWM {0, 5, 20, 35}；
 *   - 0 档走关 LED0 -> PWM=0 -> STANDBY 路径，深睡/锁屏调用 suspend 进待机；
 *   - 开机先以只读探测确认 0x45 应答且 chip id 为 0x09；无芯片（E2）时立即释放
 *     I2C 总线并进入无背光模式，后续控制调用直接空操作。
 */
#include <stdbool.h>
#include <stdint.h>

#define MP_BL_LEVEL_MAX 3
#define MP_BL_LEVEL_COUNT 4

/* 初始化并探测 AW2023，有背光则应用开机档位（0..3）。返回 0 成功，-1 无芯片或总线失败。 */
int mp_backlight_init(uint8_t level);

/* 当前设备是否探测到 AW2023 背光芯片（true = E2S 有背光，false = E2 无背光）。 */
bool mp_backlight_supported(void);

/* 设置背光档位（0..3 截断/夹紧）。无背光或 I2C 错误返回 -1（非致命）。 */
int mp_backlight_set(uint8_t level);

/* 锁屏前令 AW2023 进入 STANDBY 并释放设备句柄（保留总线便于解锁秒亮）。 */
void mp_backlight_suspend(void);

/* 进深睡前令 AW2023 进入 STANDBY 并彻底关闭 I2C 总线，消除上拉漏电。 */
void mp_backlight_deep_sleep(void);

/* 返回最近一次尝试设置的档位（0..3）。 */
uint8_t mp_backlight_level(void);
