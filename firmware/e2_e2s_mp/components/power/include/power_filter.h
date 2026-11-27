#pragma once
/* power_filter.h — 电池电量充放电防抖滤波与 IR 压升补偿引擎（纯 C，无 IDF 依赖）。
 *
 * 解决锂电池实机采样的三类典型抖动：
 *   1) 单次 ADC 毛刺与瞬态带载压降：一阶低通 IIR + 放电单调不增 + 步长限幅；
 *   2) 插入充电线瞬间内阻压升（I_chg * R_int ≈ 100~150 mV 导致虚涨 ~20%）：
 *      按插电前静置电压自适应标定 IR 压升偏置，并在恒压 CV 区（4080..4180 mV）
 *      随充电电流线性衰减归零，插电瞬间冻结显示百分比实现 0% 跳变；
 *   3) 拔掉充电线瞬间电压回落与极化恢复：扣除虚电后与开路电压无缝衔接，
 *      叠加 15 s 极化过渡窗口与步长限幅，彻底消除拔线瞬间掉电 20%。
 */
#include <stdbool.h>
#include <stdint.h>

#define POWER_FILTER_RTC_MAGIC     0x50574231u /* "PWB1" */

#define POWER_BAT_EMPTY_MV         3500        /* 0% 基准电池电压 (mV) */
#define POWER_BAT_SPAN_MV          600         /* 3500..4100 mV 映射 0..100% */
#define POWER_BAT_OFFSET_MV        25          /* 原厂固定补偿 (mV) */

#define POWER_CHG_OFFSET_DEFAULT_MV 120        /* 插线冷启动缺省充电 IR 压升补偿 (mV) */
#define POWER_CHG_OFFSET_MIN_MV     40         /* 自适应 IR 补偿下限 (mV) */
#define POWER_CHG_OFFSET_MAX_MV     220        /* 自适应 IR 补偿上限 (mV) */
#define POWER_CHG_CV_START_MV       4080       /* 进入恒压 CV 衰减区起始端电压 (mV) */
#define POWER_CHG_CV_FULL_MV        4180       /* 满电截止端电压，补偿衰减为 0 (mV) */

#define POWER_CHG_SETTLE_MS         15000u     /* 插拔电后极化静置过渡窗口 (ms) */
#define POWER_STEP_PERIOD_MS        30000u     /* 常规显示步进最小间隔 (ms) */
#define POWER_DISP_STEP_DOWN_MAX    2u         /* 常规放电单周期最大降幅 (%) */
#define POWER_DISP_STEP_UP_MAX      2u         /* 常规充电单周期最大升幅 (%) */
#define POWER_CRIT_RAW_PCT          3u         /* 真低电安全快车道原始电量阈值 (%) */
#define POWER_CRIT_CONFIRM_NEED     2u         /* 真低电快速下探所需连续确认次数 */

/* 跨深睡持久化的电量滤波状态（建议放置于 RTC_DATA_ATTR）。 */
typedef struct {
    uint32_t magic;
    uint16_t filt_mv;        /* 滤波后的等效开路电压 (mV) */
    uint16_t last_idle_mv;   /* 最近稳定未充电时的电压 (mV) */
    uint16_t chg_offset_mv;  /* 当前充电 IR 压升补偿值 (mV) */
    uint8_t disp_pct;        /* 对外稳定显示的电量百分比 (0..100) */
    uint8_t was_charging;    /* 上一次采样时的充电状态 (0/1) */
    uint8_t valid;           /* 1 = 滤波状态有效 */
    uint8_t crit_streak;     /* 连续命中危急低电计数 */
} power_filter_rtc_t;

/* 单次开机周期内的易失运行时时间戳（冷启动/深睡唤醒时清零）。 */
typedef struct {
    uint64_t chg_edge_ms;    /* 最近一次充电状态翻转的单调毫秒时刻 */
    uint64_t last_step_ms;   /* 上一次百分比步进的单调毫秒时刻 */
    bool chg_edge_valid;     /* 是否有有效插拔电边沿 */
    bool edge_recal_pending; /* 插电后待首采确认充电 IC 满流压升 */
    bool has_stepped;        /* 本次开机内是否已有步进时间戳 */
    bool first_after_boot;   /* 本次开机后尚未完成首次采样 */
} power_filter_run_t;

/* 按原厂线性公式将等效电池电压 (mV) 换算为 0..100%。 */
uint8_t power_filter_raw_percent(int vbat_mv);

/* 计算指定端电压下（含 CV 恒压线性衰减）的实际充电 IR 压升补偿量 (mV)。 */
int power_filter_effective_chg_offset(int vbat_mv, int base_offset_mv);

/* 开机初始化运行期上下文，并在 RTC 状态无效时复位持久化状态。 */
void power_filter_boot_init(power_filter_rtc_t *rtc, power_filter_run_t *run);

/* 喂入一次有效电池电压 (mV)、当前充电状态与单调毫秒时间戳，返回平滑稳定电量 (0..100)。 */
uint8_t power_filter_update(power_filter_rtc_t *rtc,
                            power_filter_run_t *run,
                            int vbat_mv,
                            bool charging,
                            uint64_t now_ms);
