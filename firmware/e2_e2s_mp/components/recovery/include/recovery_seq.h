#pragma once
/* recovery_seq.h — otadata 序号/扇区选择的纯函数(PC 可单测)。
 *
 * 两条记录的 ota_seq 与“是否可用”由调用方从 otadata 读出后传入:
 * 可用 = seq != 0xFFFFFFFF、CRC 正确、state 不是 INVALID/ABORTED。
 * 规则与 ESP-IDF 一致:slot = (seq - 1) % 2;新 seq 必须大于现有最大
 * seq(含不可用记录,保证单调),并落在目标槽;写“不持有最高**可用** seq”
 * 的扇区,保留一条真正可启动的回退记录。
 *
 * 原厂 OTA 可能产生重复 seq(本机 dump 是 43/擦除态;历史上有过 43/43),
 * 所以不能用“两条记录成对”假设。
 */
#include <stdbool.h>
#include <stdint.h>

#define RECOVERY_OTA_SLOTS 2u

static inline bool recovery_next_seq(const uint32_t stored[RECOVERY_OTA_SLOTS],
                                     const bool valid[RECOVERY_OTA_SLOTS],
                                     unsigned target_slot,
                                     uint32_t *out_seq, unsigned *out_sector)
{
    if (!stored || !valid || !out_seq || !out_sector ||
        target_slot >= RECOVERY_OTA_SLOTS) {
        return false;
    }

    uint32_t max_seq = 0;
    int high_valid = -1;
    for (unsigned i = 0; i < RECOVERY_OTA_SLOTS; i++) {
        if (stored[i] == 0xFFFFFFFFu) {
            continue;
        }
        /* 单调性对“所有未擦除记录”取最大,避免新 seq 低于某条旧记录。 */
        if (stored[i] > max_seq) {
            max_seq = stored[i];
        }
        if (valid[i] && (high_valid < 0 || stored[i] > stored[high_valid])) {
            high_valid = (int)i;
        }
    }

    uint32_t seq = max_seq + 1;
    if (seq == 0 || seq == 0xFFFFFFFFu) {
        return false;
    }
    while ((seq - 1) % RECOVERY_OTA_SLOTS != target_slot) {
        seq++;
        if (seq == 0 || seq == 0xFFFFFFFFu) {
            return false;
        }
    }

    *out_seq = seq;
    /* 保留最高可用序号所在扇区;没有可用记录时没有值得保留的回退,写扇区 0。 */
    if (high_valid == 0) {
        *out_sector = 1u;
    } else if (high_valid == 1) {
        *out_sector = 0u;
    } else {
        *out_sector = 0u;
    }
    return true;
}
