/* power_battery.c — 电池 ADC 采样与状态。
 *
 * 依据:DESIGN.md §10.1(采样)、§10.2(门限语义)、§10.3(深睡)。
 *
 * 硬件事实:ADC1_CH0 = GPIO1,12 bit,12 dB;Vbat = pin_mv × 2;
 * GPIO45 只在采样窗口内置 1(>120 ms)再清零;GPIO5 输入低 = charging
 * (语义未确认,盘点 §8.5)。
 *
 * ADC 单位陷阱:IDF v6.1.1 的 adc_unit_t 是 {ADC_UNIT_1, ADC_UNIT_2},
 * 即 ADC_UNIT_1 == 0;写成 1 会变成 ADC_UNIT_2,而 ADC2_CHANNEL_0 是
 * GPIO11(SD CLK)。因此用 adc_oneshot_io_to_channel(GPIO1) 在运行时断言
 * unit==0 && ch==0,不匹配就置 valid=false 并返回错误(不 assert 卡死)。
 *
 * READER_ADC_OVERRIDE_MV > 0 时整条 ADC 路径不编译,直接用该 mV 作为电池
 * 电压,仅供 §15 #13 的低电验收。按 §10.1 公式 pct=(vbat+25-3500)*100/600
 * 换算,可得三档用例(详见 power_sample() 的注释):
 *   3400 mV → pct 0 → critical(启动 ALERT、上传 status 8)
 *   3495 mV → pct 3 → 仅 warning(可翻页、可上传)
 *   3800 mV → pct 54 → 正常
 */
#include "power.h"
#include "power_filter.h"

#include "board.h"
#include "driver/gpio.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_attr.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "sdkconfig.h"
#ifdef CONFIG_PM_ENABLE
#include "esp_pm.h"
#endif
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "rc_common.h"
#include "reader_cfg.h"

static const char *TAG = "power";

#define ADC_SAMPLES 8
#define VBAT_MAX_MV 4300          /* 超过视为分压/采样异常(§10.1) */

/* GPIO5 内部上拉:**推断值,需真机确认**。盘点 §8.5 没有给出该脚的外部电路,
 * 这里开内部上拉只是为了让“悬空(未接充电器)”读出确定的高电平 = 未充电;
 * 若真机测量表明外部已有上拉/分压,或内部上拉会干扰充电检测,用
 * -DPOWER_CHARGE_DET_PULLUP=0 关掉,不要凭猜测改默认值。 */
#ifndef POWER_CHARGE_DET_PULLUP
#define POWER_CHARGE_DET_PULLUP 1
#endif

#if POWER_CHARGE_DET_PULLUP
#define CHARGE_DET_PULL GPIO_PULLUP_ENABLE
#else
#define CHARGE_DET_PULL GPIO_PULLUP_DISABLE
#endif

static power_status_t s_status;
static uint16_t s_hist_mv;        /* 上一份合理读数,用于 vbat>4300 时沿用 */
static bool s_hist_valid;
RTC_DATA_ATTR static power_filter_rtc_t s_bat_rtc; /* 跨深睡持久化的电量防抖滤波状态 */
static power_filter_run_t s_bat_run;               /* 单次开机周期内的插拔与步进时间戳 */
static volatile bool s_chg_irq_pending; /* GPIO5 充电电平翻转中断待处理标志 */
static bool s_chg_irq_ok;         /* GPIO5 中断与轻睡唤醒是否已就绪 */
static bool s_chg_armed_state;    /* 当前中断/轻睡唤醒所对应的稳定充电状态 */
static void (*s_charge_wake_cb)(void);
#ifdef CONFIG_PM_ENABLE
static esp_pm_lock_handle_t s_pm_lock; /* 采样分压期间禁止自动轻睡 */
#endif

#if READER_ADC_OVERRIDE_MV <= 0
static adc_oneshot_unit_handle_t s_adc;
static adc_cali_handle_t s_cali;
#endif

static bool charge_detect(void)
{
    /* 低 = charging;开内部上拉,悬空(无充电器)时读出确定的高电平。 */
    return gpio_get_level(BOARD_CHARGE_DET_GPIO) == 0;
}

/* GPIO5 电平翻转中断服务函数：立即禁中断防止电平抖动风暴，并唤醒应用任务防抖确认。 */
static void IRAM_ATTR charge_isr(void *arg)
{
    (void)arg;
    gpio_intr_disable((gpio_num_t)BOARD_CHARGE_DET_GPIO);
    s_chg_irq_pending = true;
    void (*cb)(void) = s_charge_wake_cb;
    if (cb) {
        cb();
    }
}

/* 按当前稳定充电状态装填反相电平中断与自动轻睡唤醒：
 *   - 未充电(高电平)：监听 LOW_LEVEL，插入充电器瞬间唤醒；
 *   - 充电中(低电平)：监听 HIGH_LEVEL，避免插电时阻止自动轻睡且拔出瞬间唤醒。 */
static void charge_irq_arm(bool cur_charging)
{
    if (!s_chg_irq_ok) {
        return;
    }
    gpio_int_type_t want =
        cur_charging ? GPIO_INTR_HIGH_LEVEL : GPIO_INTR_LOW_LEVEL;
    (void)gpio_intr_disable((gpio_num_t)BOARD_CHARGE_DET_GPIO);
    (void)gpio_wakeup_enable((gpio_num_t)BOARD_CHARGE_DET_GPIO, want);
    s_chg_armed_state = cur_charging;
    (void)gpio_intr_enable((gpio_num_t)BOARD_CHARGE_DET_GPIO);
}

void power_set_charge_wake_cb(void (*cb)(void))
{
    s_charge_wake_cb = cb;
    if (cb && s_chg_irq_pending) {
        cb();
    }
}

bool power_charge_poll(bool *changed)
{
    bool prev = s_status.charging;
    if (s_chg_irq_pending) {
        s_chg_irq_pending = false;
        /* 对齐官方 pm_charge_event_send 防抖逻辑，跳过 USB-C 插拔瞬间抖动。 */
        vTaskDelay(pdMS_TO_TICKS(30));
        bool now = charge_detect();
        s_status.charging = now;
        charge_irq_arm(now);
    } else {
        bool now = charge_detect();
        if (now != s_status.charging ||
            (s_chg_irq_ok && now != s_chg_armed_state)) {
            vTaskDelay(pdMS_TO_TICKS(20));
            now = charge_detect();
            s_status.charging = now;
            charge_irq_arm(now);
        }
    }
    if (changed) {
        *changed = (s_status.charging != prev);
    }
    return s_status.charging;
}

#if READER_ADC_OVERRIDE_MV <= 0
static int adc_setup(void)
{
    adc_unit_t unit = (adc_unit_t)-1;
    adc_channel_t ch = (adc_channel_t)-1;
    esp_err_t err = adc_oneshot_io_to_channel(BOARD_ADC_GPIO, &unit, &ch);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "GPIO%d is not an ADC pad: %s", BOARD_ADC_GPIO, esp_err_to_name(err));
        return (int)err;
    }
    if ((int)unit != BOARD_ADC_UNIT || (int)ch != BOARD_ADC_CHANNEL) {
        ESP_LOGE(TAG, "ADC map mismatch: GPIO%d -> unit=%d ch=%d, expected unit=%d ch=%d",
                 BOARD_ADC_GPIO, (int)unit, (int)ch, BOARD_ADC_UNIT, BOARD_ADC_CHANNEL);
        return ESP_ERR_INVALID_STATE;
    }

    adc_oneshot_unit_init_cfg_t ucfg = {
        .unit_id = (adc_unit_t)BOARD_ADC_UNIT,
        .clk_src = 0, /* 0 = 驱动默认时钟源 */
        .ulp_mode = ADC_ULP_MODE_DISABLE,
    };
    err = adc_oneshot_new_unit(&ucfg, &s_adc);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "adc_oneshot_new_unit failed: %s", esp_err_to_name(err));
        s_adc = NULL;
        return (int)err;
    }

    adc_oneshot_chan_cfg_t ccfg = {
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_12,
    };
    err = adc_oneshot_config_channel(s_adc, (adc_channel_t)BOARD_ADC_CHANNEL, &ccfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "adc_oneshot_config_channel failed: %s", esp_err_to_name(err));
        adc_oneshot_del_unit(s_adc);
        s_adc = NULL;
        return (int)err;
    }

    adc_cali_curve_fitting_config_t ccal = {
        .unit_id = (adc_unit_t)BOARD_ADC_UNIT,
        .chan = (adc_channel_t)BOARD_ADC_CHANNEL,
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_12,
    };
    err = adc_cali_create_scheme_curve_fitting(&ccal, &s_cali);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "curve-fitting calibration unavailable: %s", esp_err_to_name(err));
        adc_oneshot_del_unit(s_adc);
        s_adc = NULL;
        s_cali = NULL;
        return (int)err;
    }

    ESP_LOGI(TAG, "ADC1_CH0 (GPIO%d) ready: unit=%d ch=%d, %d bit, 12 dB, curve fitting",
             BOARD_ADC_GPIO, (int)unit, (int)ch, 12);
    return 0;
}
#endif /* READER_ADC_OVERRIDE_MV <= 0 */

/* 把一次电池电压读数套上门限/量程规则,写进 s_status。 */
static void apply_vbat(int vbat_mv)
{
    if (vbat_mv > VBAT_MAX_MV) {
        ESP_LOGW(TAG, "vbat=%d mV out of range (>%d)", vbat_mv, VBAT_MAX_MV);
        if (!s_hist_valid) {
            /* 首采即超量程:不触发任何门限(§10.1/§10.2)。 */
            s_status.mv = (uint16_t)rc_clamp(vbat_mv, 0, 65535);
            s_status.percent = 0;
            s_status.valid = false;
            return;
        }
        vbat_mv = s_hist_mv; /* 有历史则沿用上一次合理值 */
    }

    s_status.mv = (uint16_t)vbat_mv;
#if READER_ADC_OVERRIDE_MV > 0
    s_status.percent = power_filter_raw_percent(vbat_mv);
#else
    s_status.percent = power_filter_update(&s_bat_rtc, &s_bat_run, vbat_mv,
                                           s_status.charging, rc_now_ms());
#endif
    s_status.valid = true;
    s_hist_mv = (uint16_t)vbat_mv;
    s_hist_valid = true;
}

int power_init(void)
{
    s_status = (power_status_t){0};
    s_hist_mv = 0;
    s_hist_valid = false;
    power_filter_boot_init(&s_bat_rtc, &s_bat_run);

    /* GPIO45:采样窗口使能,平时保持 0(红线:GPIO45 只瞬时置 1)。 */
    gpio_config_t en_cfg = {
        .pin_bit_mask = 1ull << BOARD_VBAT_EN_GPIO,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&en_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "VBAT_EN GPIO%d config failed: %s", BOARD_VBAT_EN_GPIO,
                 esp_err_to_name(err));
        return (int)err;
    }
    gpio_set_level(BOARD_VBAT_EN_GPIO, 0);

    /* GPIO5:只读。上拉仅为让悬空时读出“未充电”,不驱动外部电路。
     * 上拉开关是推断项,见 POWER_CHARGE_DET_PULLUP 的说明。 */
    gpio_config_t chg_cfg = {
        .pin_bit_mask = 1ull << BOARD_CHARGE_DET_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = CHARGE_DET_PULL,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    err = gpio_config(&chg_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "CHARGE_DET GPIO%d config failed: %s", BOARD_CHARGE_DET_GPIO,
                 esp_err_to_name(err));
        return (int)err;
    }
    s_status.charging = charge_detect();
    s_chg_irq_pending = false;
    s_chg_irq_ok = false;

    /* 注册 GPIO5 充电插拔硬件中断与轻睡唤醒（对齐官方 battery_charge_ping_irq_register）；
     * 若 ISR 服务不可用则自动退化为轮询检测。 */
    esp_err_t ierr = gpio_install_isr_service(0);
    if (ierr == ESP_OK || ierr == ESP_ERR_INVALID_STATE) {
        (void)gpio_isr_handler_remove((gpio_num_t)BOARD_CHARGE_DET_GPIO);
        if (gpio_isr_handler_add((gpio_num_t)BOARD_CHARGE_DET_GPIO, charge_isr,
                                 NULL) == ESP_OK &&
            esp_sleep_enable_gpio_wakeup() == ESP_OK) {
            s_chg_irq_ok = true;
            charge_irq_arm(s_status.charging);
            ESP_LOGI(TAG, "charge detect IRQ ready on GPIO%d (charging=%d)",
                     BOARD_CHARGE_DET_GPIO, (int)s_status.charging);
        } else {
            ESP_LOGW(TAG, "charge detect IRQ setup incomplete; using poll fallback");
        }
    } else {
        ESP_LOGW(TAG, "gpio_install_isr_service failed: %s; charge poll fallback",
                 esp_err_to_name(ierr));
    }

#if READER_ADC_OVERRIDE_MV > 0
    ESP_LOGW(TAG, "READER_ADC_OVERRIDE_MV=%d: ADC path disabled (acceptance build)",
             (int)READER_ADC_OVERRIDE_MV);
#else
    int aerr = adc_setup();
    if (aerr != 0) {
        s_status.valid = false;
        s_status.percent = 0;
        return aerr;
    }
#endif

    /* 首采放在 init,保证 app 在第一次 power_tick() 之前就能拿到有效电量
     * (启动 critical → ALERT 的判定依赖它,§7.3/§10.2)。 */
#ifdef CONFIG_PM_ENABLE
    if (!s_pm_lock) {
        (void)esp_pm_lock_create(ESP_PM_NO_LIGHT_SLEEP, 0, "adc", &s_pm_lock);
    }
#endif
    power_sample();
    return 0;
}

void power_sample(void)
{
    bool chg = charge_detect();
    s_status.charging = chg;
    if (s_chg_irq_ok && !s_chg_irq_pending && chg != s_chg_armed_state) {
        charge_irq_arm(chg);
    }

#if READER_ADC_OVERRIDE_MV > 0
    /* 验收路径:跳过 ADC 与 GPIO45,直接把宏值当作电池 mV。
     * 这里没有真实分压点读数,pin_mv 按“分压比 1:2”反推,只为让 INFO/日志
     * 字段非零且自洽(§15 #16 只约束真实 ADC 路径)。
     *
     * §15 #13 的取值(公式见 §10.1,门限见 §10.2):
     *   3400 → pct 0  → critical:启动 ALERT,上传 0x90 status=8
     *   3495 → pct 3  → 仅 warning:可翻页、可上传(3 在 [2,5) 内)
     *   3800 → pct 54 → 正常(不是“仅警告”;原文用例与冻结公式矛盾)
     * warning 且非 critical 的电压窗口是 3487..3504 mV(pct 2..4)。 */
    int vbat = READER_ADC_OVERRIDE_MV;
    s_status.pin_mv = (uint32_t)(vbat / 2);
    apply_vbat(vbat);
    ESP_LOGI(TAG, "override: pin=%u mV vbat=%d mV pct=%u%% valid=%d charging=%d",
             (unsigned)s_status.pin_mv, vbat, s_status.percent, s_status.valid,
             s_status.charging);
#else
    if (!s_adc || !s_cali) {
        s_status.valid = false;
        s_status.percent = 0;
        s_status.pin_mv = 0;   /* 本周期没有 ADC 读数 */
        return;
    }

#ifdef CONFIG_PM_ENABLE
    if (s_pm_lock) {
        (void)esp_pm_lock_acquire(s_pm_lock);
    }
#endif
    gpio_set_level(BOARD_VBAT_EN_GPIO, 1);
    vTaskDelay(pdMS_TO_TICKS(READER_ADC_SETTLE_MS));

    int sum = 0;
    int min_mv = 0;
    int max_mv = 0;
    int n = 0;
    for (int i = 0; i < ADC_SAMPLES; i++) {
        int raw = 0;
        int mv = 0;
        esp_err_t err = adc_oneshot_read(s_adc, (adc_channel_t)BOARD_ADC_CHANNEL, &raw);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "adc_oneshot_read failed: %s", esp_err_to_name(err));
            continue;
        }
        err = adc_cali_raw_to_voltage(s_cali, raw, &mv);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "adc_cali_raw_to_voltage failed: %s", esp_err_to_name(err));
            continue;
        }
        if (n == 0) {
            min_mv = mv;
            max_mv = mv;
        } else {
            if (mv < min_mv) {
                min_mv = mv;
            }
            if (mv > max_mv) {
                max_mv = mv;
            }
        }
        sum += mv;
        n++;
    }

    gpio_set_level(BOARD_VBAT_EN_GPIO, 0);
#ifdef CONFIG_PM_ENABLE
    if (s_pm_lock) {
        (void)esp_pm_lock_release(s_pm_lock);
    }
#endif

    if (n == 0) {
        ESP_LOGE(TAG, "all %d ADC samples failed", ADC_SAMPLES);
        s_status.valid = false;
        s_status.percent = 0;
        s_status.pin_mv = 0;   /* 本周期没有 ADC 读数 */
        return;
    }

    /* 有效采样数 >= 4 时去掉 1 个最高毛刺与 1 个最低毛刺再取四舍五入均值。 */
    int pin_mv = (n >= 4)
                     ? ((sum - min_mv - max_mv + (n - 2) / 2) / (n - 2))
                     : ((sum + n / 2) / n);
    int vbat_mv = pin_mv * 2;
    /* 先记本次分压点读数,再套门限:apply_vbat() 可能因 vbat>4300 沿用历史
     * 值,那时 pin_mv 仍应反映本次真实采样(§15 #16 要与万用表比对)。 */
    s_status.pin_mv = (uint32_t)pin_mv;
    apply_vbat(vbat_mv);
    ESP_LOGI(TAG, "adc pin=%d mV vbat=%d mV raw=%u%% pct=%u%% off=%u mV valid=%d charging=%d",
             pin_mv, s_status.mv, power_filter_raw_percent(s_status.mv),
             s_status.percent, (unsigned)s_bat_rtc.chg_offset_mv,
             s_status.valid, s_status.charging);
#endif /* READER_ADC_OVERRIDE_MV > 0 */
}

const power_status_t *power_get(void)
{
    return &s_status;
}

int power_enter_deep_sleep(void)
{
    /* v1:READER_ENABLE_DEEP_SLEEP=0,不实现 DFS/深睡(§10.3)。
     * 即使有人在构建时强开该宏,这里也宁可返回“不支持”而不是假装成功。 */
#if READER_ENABLE_DEEP_SLEEP
    ESP_LOGE(TAG, "deep sleep requested but not implemented in v1");
#endif
    return ESP_ERR_NOT_SUPPORTED;
}
