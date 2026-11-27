/* power_filter.c — 电池电量充放电防抖滤波与 IR 压升补偿实现。 */
#include "power_filter.h"

#include <string.h>

#include "rc_common.h"

#define MV_CLAMP_LO 2500
#define MV_CLAMP_HI 4500

/* 深睡期间插入充电线后，若扣除偏置的电量较睡前显著增加（>=6%），说明已持续充入可观电量。 */
#define WAKE_CHG_SYNC_MIN_DELTA 6u
/* 深睡期间拔掉充电线后，与静置开路电量的允许无感误差窗（<=4% 保持原值）。 */
#define WAKE_UNPLUG_DEADBAND_PCT 4u

uint8_t power_filter_raw_percent(int vbat_mv)
{
    int v = vbat_mv + POWER_BAT_OFFSET_MV;
    int pct = (v - POWER_BAT_EMPTY_MV) * 100 / POWER_BAT_SPAN_MV;
    return (uint8_t)rc_clamp(pct, 0, 100);
}

int power_filter_effective_chg_offset(int vbat_mv, int base_offset_mv)
{
    if (base_offset_mv <= 0 || vbat_mv >= POWER_CHG_CV_FULL_MV) {
        return 0;
    }
    if (vbat_mv <= POWER_CHG_CV_START_MV) {
        return base_offset_mv;
    }
    return base_offset_mv * (POWER_CHG_CV_FULL_MV - vbat_mv) /
           (POWER_CHG_CV_FULL_MV - POWER_CHG_CV_START_MV);
}

static uint16_t clamp_mv_u16(int mv)
{
    return (uint16_t)rc_clamp(mv, MV_CLAMP_LO, MV_CLAMP_HI);
}

static uint16_t default_or_kept_offset(uint16_t cur)
{
    if (cur < POWER_CHG_OFFSET_MIN_MV || cur > POWER_CHG_OFFSET_MAX_MV) {
        return POWER_CHG_OFFSET_DEFAULT_MV;
    }
    return cur;
}

void power_filter_boot_init(power_filter_rtc_t *rtc, power_filter_run_t *run)
{
    if (run) {
        memset(run, 0, sizeof(*run));
        run->first_after_boot = true;
    }
    if (rtc && (rtc->magic != POWER_FILTER_RTC_MAGIC || !rtc->valid)) {
        memset(rtc, 0, sizeof(*rtc));
    }
}

uint8_t power_filter_update(power_filter_rtc_t *rtc,
                            power_filter_run_t *run,
                            int vbat_mv,
                            bool charging,
                            uint64_t now_ms)
{
    if (!rtc || !run) {
        return power_filter_raw_percent(vbat_mv);
    }

    /* 1) 冷启动无历史：按当前充电态建立初始等效开路电压与显示电量。 */
    if (rtc->magic != POWER_FILTER_RTC_MAGIC || !rtc->valid) {
        rtc->magic = POWER_FILTER_RTC_MAGIC;
        rtc->valid = 1;
        rtc->was_charging = charging ? 1u : 0u;
        rtc->chg_offset_mv = POWER_CHG_OFFSET_DEFAULT_MV;

        int off = charging ? power_filter_effective_chg_offset(vbat_mv, rtc->chg_offset_mv) : 0;
        int ocv_mv = vbat_mv - off;
        rtc->filt_mv = clamp_mv_u16(ocv_mv);
        rtc->last_idle_mv = charging ? rtc->filt_mv : clamp_mv_u16(vbat_mv);
        rtc->disp_pct = power_filter_raw_percent(ocv_mv);
        rtc->crit_streak = (!charging && rtc->disp_pct < POWER_CRIT_RAW_PCT) ? 1u : 0u;

        run->first_after_boot = false;
        run->last_step_ms = now_ms;
        run->has_stepped = true;
        run->chg_edge_valid = false;
        return rtc->disp_pct;
    }

    rtc->chg_offset_mv = default_or_kept_offset(rtc->chg_offset_mv);

    /* 2) 跨深睡唤醒后的首次采样：允许反映长睡期间的真实充放电变化，同时避免插拔瞬间跳变。 */
    if (run->first_after_boot) {
        run->first_after_boot = false;
        run->last_step_ms = now_ms;
        run->has_stepped = true;

        bool prev_chg = (rtc->was_charging != 0);
        if (charging && !prev_chg) {
            /* 深睡期间插入充电线：扣除充电压升偏置，仅当真充入显著电量时才上调。 */
            rtc->was_charging = 1;
            rtc->crit_streak = 0;
            run->chg_edge_ms = now_ms;
            run->chg_edge_valid = true;

            int ocv_mv = vbat_mv - power_filter_effective_chg_offset(vbat_mv, rtc->chg_offset_mv);
            uint8_t ocv_pct = power_filter_raw_percent(ocv_mv);
            rtc->filt_mv = clamp_mv_u16(ocv_mv);
            if ((int)ocv_pct >= (int)rtc->disp_pct + (int)WAKE_CHG_SYNC_MIN_DELTA) {
                rtc->disp_pct = ocv_pct;
            }
            return rtc->disp_pct;
        }
        if (!charging && prev_chg) {
            /* 深睡期间拔掉充电线：当前电压已静置稳定，超误差窗才对齐真实开路电量。 */
            rtc->was_charging = 0;
            run->chg_edge_ms = now_ms;
            run->chg_edge_valid = true;

            uint8_t ocv_pct = power_filter_raw_percent(vbat_mv);
            int diff = (int)ocv_pct - (int)rtc->disp_pct;
            if (diff < 0) {
                diff = -diff;
            }
            if (diff > (int)WAKE_UNPLUG_DEADBAND_PCT) {
                rtc->disp_pct = ocv_pct;
            }
            rtc->filt_mv = clamp_mv_u16(vbat_mv);
            rtc->last_idle_mv = rtc->filt_mv;
            rtc->crit_streak = (ocv_pct < POWER_CRIT_RAW_PCT) ? 1u : 0u;
            return rtc->disp_pct;
        }
        if (charging) {
            /* 持续充电跨深睡醒来：只升不降，充满可直接同步到 100%。 */
            rtc->crit_streak = 0;
            int ocv_mv = vbat_mv - power_filter_effective_chg_offset(vbat_mv, rtc->chg_offset_mv);
            uint8_t ocv_pct = power_filter_raw_percent(ocv_mv);
            rtc->filt_mv = clamp_mv_u16(ocv_mv);
            if (ocv_pct > rtc->disp_pct) {
                rtc->disp_pct = ocv_pct;
            }
            return rtc->disp_pct;
        }
        /* 持续未充电跨深睡醒来：只降不升，屏蔽短睡轻微回弹，长睡真实掉电即时反映。 */
        uint8_t ocv_pct = power_filter_raw_percent(vbat_mv);
        rtc->filt_mv = clamp_mv_u16(vbat_mv);
        rtc->last_idle_mv = rtc->filt_mv;
        if (ocv_pct < rtc->disp_pct) {
            rtc->disp_pct = ocv_pct;
        }
        rtc->crit_streak = (ocv_pct < POWER_CRIT_RAW_PCT) ? 1u : 0u;
        return rtc->disp_pct;
    }

    /* 3) 运行期实时插入充电线（0 -> 1）：标定内阻压升并冻结显示电量，实现 0% 跳变。 */
    if (!rtc->was_charging && charging) {
        rtc->was_charging = 1;
        rtc->crit_streak = 0;
        run->chg_edge_ms = now_ms;
        run->chg_edge_valid = true;
        run->edge_recal_pending = true;

        int ocv_mv;
        if (rtc->last_idle_mv > 0 &&
            (int)rtc->last_idle_mv < (POWER_CHG_CV_START_MV - 40)) {
            int jump = vbat_mv - (int)rtc->last_idle_mv;
            if (jump >= POWER_CHG_OFFSET_MIN_MV) {
                rtc->chg_offset_mv = (uint16_t)rc_clamp(jump,
                                                        POWER_CHG_OFFSET_MIN_MV,
                                                        POWER_CHG_OFFSET_MAX_MV);
                ocv_mv = vbat_mv - power_filter_effective_chg_offset(vbat_mv, rtc->chg_offset_mv);
            } else {
                /* 充电芯片尚在软启动未满流：保留插电前开路电压基准，不强扣默认偏置。 */
                ocv_mv = (int)rtc->last_idle_mv;
            }
        } else {
            ocv_mv = vbat_mv - power_filter_effective_chg_offset(vbat_mv, rtc->chg_offset_mv);
            if (rtc->last_idle_mv > 0 && ocv_mv < (int)rtc->last_idle_mv) {
                ocv_mv = (int)rtc->last_idle_mv;
            }
        }
        rtc->filt_mv = clamp_mv_u16(ocv_mv);
        return rtc->disp_pct;
    }

    /* 4) 运行期实时拔掉充电线（1 -> 0）：清空旧静置基准防反复插拔污染，冻结显示并开启过渡窗。 */
    if (rtc->was_charging && !charging) {
        rtc->was_charging = 0;
        rtc->last_idle_mv = 0;
        run->chg_edge_ms = now_ms;
        run->chg_edge_valid = true;
        run->edge_recal_pending = false;
        rtc->filt_mv = clamp_mv_u16(vbat_mv);
        return rtc->disp_pct;
    }

    bool settling = run->chg_edge_valid && (now_ms >= run->chg_edge_ms) &&
                    ((now_ms - run->chg_edge_ms) < POWER_CHG_SETTLE_MS);
    bool step_due = !run->has_stepped ||
                    (now_ms >= run->last_step_ms &&
                     (now_ms - run->last_step_ms) >= POWER_STEP_PERIOD_MS);

    /* 5) 运行期稳态充电中：过渡窗及首周期内追踪充电芯片满流压升，稳态单调平滑上升。 */
    if (charging) {
        rtc->crit_streak = 0;
        if (rtc->last_idle_mv > 0 &&
            (int)rtc->last_idle_mv < (POWER_CHG_CV_START_MV - 40) &&
            vbat_mv > (int)rtc->last_idle_mv) {
            int jump = vbat_mv - (int)rtc->last_idle_mv;
            if (settling) {
                if (jump > (int)rtc->chg_offset_mv) {
                    rtc->chg_offset_mv = (uint16_t)rc_clamp(jump,
                                                            POWER_CHG_OFFSET_MIN_MV,
                                                            POWER_CHG_OFFSET_MAX_MV);
                }
            } else if (run->edge_recal_pending) {
                /* 首次稳态采样若距插电 <= 75 s 且压升仍突增超 20 mV，补齐慢软启动偏置。 */
                if (run->chg_edge_valid && now_ms >= run->chg_edge_ms &&
                    (now_ms - run->chg_edge_ms) <= 75000u &&
                    jump > (int)rtc->chg_offset_mv + 20) {
                    int est = jump - 6;
                    rtc->chg_offset_mv = (uint16_t)rc_clamp(est,
                                                            POWER_CHG_OFFSET_MIN_MV,
                                                            POWER_CHG_OFFSET_MAX_MV);
                }
                run->edge_recal_pending = false;
            }
        } else if (!settling) {
            run->edge_recal_pending = false;
        }

        int ocv_mv = vbat_mv - power_filter_effective_chg_offset(vbat_mv, rtc->chg_offset_mv);
        int filt_mv = ((int)rtc->filt_mv * 3 + ocv_mv + 2) / 4;
        rtc->filt_mv = clamp_mv_u16(filt_mv);

        if (!settling && step_due) {
            uint8_t target = power_filter_raw_percent(filt_mv);
            if (target > rtc->disp_pct) {
                uint8_t delta = (uint8_t)(target - rtc->disp_pct);
                if (delta > POWER_DISP_STEP_UP_MAX) {
                    delta = POWER_DISP_STEP_UP_MAX;
                }
                rtc->disp_pct = (uint8_t)(rtc->disp_pct + delta);
                run->last_step_ms = now_ms;
                run->has_stepped = true;
            }
        }
        return rtc->disp_pct;
    }

    /* 6) 运行期稳态放电中：IIR 滤波 + 严格单调不增 + 步长限幅 + 危急低电安全快车道。 */
    int filt_mv = ((int)rtc->filt_mv * 3 + vbat_mv + 2) / 4;
    rtc->filt_mv = clamp_mv_u16(filt_mv);
    if (!settling) {
        rtc->last_idle_mv = rtc->filt_mv;
    }

    uint8_t raw_pct = power_filter_raw_percent(vbat_mv);
    if (raw_pct < POWER_CRIT_RAW_PCT) {
        if (rtc->crit_streak < 255u) {
            rtc->crit_streak++;
        }
    } else {
        rtc->crit_streak = 0;
    }

    /* 连续两次确认真实危急低电时绕过慢步长限制，确保低电保护及时生效。 */
    if (!settling && rtc->crit_streak >= POWER_CRIT_CONFIRM_NEED) {
        if (raw_pct < rtc->disp_pct) {
            rtc->disp_pct = raw_pct;
            run->last_step_ms = now_ms;
            run->has_stepped = true;
        }
        return rtc->disp_pct;
    }

    if (!settling && step_due) {
        uint8_t target = power_filter_raw_percent(filt_mv);
        if (target < rtc->disp_pct) {
            uint8_t delta = (uint8_t)(rtc->disp_pct - target);
            if (delta > POWER_DISP_STEP_DOWN_MAX) {
                delta = POWER_DISP_STEP_DOWN_MAX;
            }
            rtc->disp_pct = (uint8_t)(rtc->disp_pct - delta);
            run->last_step_ms = now_ms;
            run->has_stepped = true;
        }
    }
    return rtc->disp_pct;
}
