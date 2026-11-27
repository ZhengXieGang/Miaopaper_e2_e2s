#pragma once
/* power.h — frozen power API (DESIGN.md §10, §17.2)
 *
 * ADC1_CH0 = GPIO1 (BOARD_ADC_UNIT is 0!), 12-bit, 12 dB, GPIO45 high for
 * READER_ADC_SETTLE_MS before 8 samples; Vbat = pin * 2;
 * pct = clamp((Vbat + 25 - 3500) * 100 / 600, 0, 100).
 */
#include <stdbool.h>
#include <stdint.h>

typedef struct {
    uint16_t mv;          /* battery mV after the x2 divider */
    uint8_t percent;
    bool valid;
    bool charging;        /* GPIO5 低电平 = 正在充电（对齐官方 battery_check.c） */
    /* 新增字段(仅供 §15 #16 的真实 ADC 验收):最近一次 ADC 采样的分压点
     * 电压 mV,即 adc_cali_raw_to_voltage() 的 8 次平均;正常情况下
     * mv = pin_mv × 2,只有 vbat 超量程而沿用历史值时两者不相等。
     * 本周期没有 ADC 读数(初始化失败/8 次采样全失败)时为 0;
     * READER_ADC_OVERRIDE_MV 构建下没有真实采样,填 override_mv / 2。 */
    uint32_t pin_mv;
} power_status_t;

int power_init(void);
void power_sample(void);
const power_status_t *power_get(void);

/* Called from the app task once per second: schedules the 60 s sample and
 * applies the warn/critical hysteresis. */
void power_tick(void);

/* 注册 GPIO5 充电状态跳变时的唤醒回调（可能从 ISR 上下文调用）。 */
void power_set_charge_wake_cb(void (*cb)(void));

/* 消费 GPIO5 充电中断并做防抖确认，刷新 charging 状态并重装反相轻睡唤醒。
 * 返回最新充电状态；changed 非空时写入本次调用是否发生跳变。 */
bool power_charge_poll(bool *changed);

bool power_low_warn(void);       /* < 5 %, clears > 7 % */
bool power_low_critical(void);   /* < 2 %, clears > 5 % */

/* v1 returns ESP_ERR_NOT_SUPPORTED (READER_ENABLE_DEEP_SLEEP=0, §10.3). */
int power_enter_deep_sleep(void);
