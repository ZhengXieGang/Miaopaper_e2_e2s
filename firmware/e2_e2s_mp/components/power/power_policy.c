/* power_policy.c — 60 s 采样节拍与低电迟滞(DESIGN.md §10.2)。
 *
 * 门限(带迟滞,只在本份读数 valid 时评估):
 *   warning   pct < 5 置位,> 7 清除
 *   critical  pct < 2 置位,> 5 清除
 * 两档都要求 valid:warn 的“pct < 5%”只有在 valid 读数上才有物理意义,
 * 否则 percent 的缺省 0 会在每次 ADC 失败时误报“电量低”。
 * 无效读数(ADC 失败 / 超量程且无历史)不改变已有迟滞状态:已经置位的
 * warning/critical 保持置位,未置位也不会被 percent=0 误置位。
 *
 * 策略边界:本文件只产生两个布尔标志。低电在阅读中只用于状态栏警告,
 * 不禁止翻页;启动时若 critical 为真,由 app 进 ALERT 并拒绝上传(§10.2)。
 */
#include "power.h"

#include "esp_log.h"
#include "rc_common.h"
#include "reader_cfg.h"

static const char *TAG = "power";

static bool s_warn;
static bool s_crit;

/* 上次采样的时间。0 表示“power_init 的首采就是最近一次”:第一次
 * power_tick 距启动远小于 60 s,自然不会重复采;之后每 60 s 一次。 */
static uint64_t s_last_sample_ms;

/* 用当前读数推进迟滞。对同一份读数反复调用是幂等的。 */
static void policy_sync(void)
{
    const power_status_t *st = power_get();
    bool warn = s_warn;
    bool crit = s_crit;

    if (!st->valid) {
        /* 无效读数(ADC 失败 / 超量程且无历史)不改变迟滞状态:percent 的
         * 0 只是缺省值,不能当成真实低电;已经 critical 的设备保持 critical
         * (电量未知时拒绝上传比误判满电更安全)。 */
        return;
    }

    int pct = (int)st->percent;
    if (!s_warn) {
        if (pct < READER_LOW_WARN_PCT) {
            s_warn = true;
        }
    } else if (pct > READER_LOW_WARN_EXIT_PCT) {
        s_warn = false;
    }

    if (!s_crit) {
        if (pct < READER_LOW_CRIT_PCT) {
            s_crit = true;
        }
    } else if (pct > READER_LOW_CRIT_EXIT_PCT) {
        s_crit = false;
    }

    if (warn != s_warn || crit != s_crit) {
        ESP_LOGW(TAG, "battery %u%%: warn=%d critical=%d", st->percent, (int)s_warn,
                 (int)s_crit);
    }
}

void power_tick(void)
{
    uint64_t now = rc_now_ms();

    if (now - s_last_sample_ms >= READER_BATTERY_PERIOD_MS) {
        s_last_sample_ms = now;
        power_sample();
    }
    policy_sync();
}

bool power_low_warn(void)
{
    /* 同步一次:app 可能在第一次 power_tick() 之前就用 init 的首采做判定。 */
    policy_sync();
    return s_warn;
}

bool power_low_critical(void)
{
    policy_sync();
    return s_crit;
}
