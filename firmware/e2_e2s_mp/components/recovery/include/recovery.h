#pragma once
/* recovery.h — 切回原厂启动槽(本工程唯一允许写 otadata 的模块)。
 *
 * 只改 otadata 的启动选择,让它指向另一个槽里**已经存在**的原厂 app;
 * 不接收、不写入、不修改任何固件镜像。目标槽必须通过
 * esp_image_verify()(magic/段/校验和/镜像尾部 SHA-256)且 app descriptor
 * 的 project_name 为 "paperang_e2"。
 *
 * 安全边界:
 *  - 不写 bootloader / 分区表 / NVS / phy_init / 运行槽 / 原厂镜像;
 *  - 只擦写 otadata 中“不持有最高**可用** seq”的那一个 4 KiB 扇区,保留
 *    一条 CRC/state 有效的记录作回退;若两条都不可用,则没有可回退槽;
 *  - 目标镜像必须带 appended SHA-256,且校验期间不能有调试器连接
 *    (app 构建在 JTAG attached 时会跳过完整性检查),否则拒绝;
 *  - secure boot / flash encryption / anti-rollback 任一开启时拒绝执行,
 *    不做任何绕过。
 *
 * 这不是“下载并安装原厂固件”:原厂镜像必须已经存在于另一个 OTA 槽。
 * 若原厂槽被覆盖或损坏,函数返回 RECOVERY_ERR_NO_STOCK,不写 flash。
 */
#include <stddef.h>

typedef enum {
    RECOVERY_OK = 0,
    RECOVERY_ERR_NO_STOCK = -1,    /* 另一个 OTA 槽没有有效原厂镜像 */
    RECOVERY_ERR_LAYOUT = -2,      /* 分区表 / otadata 布局不符合预期 */
    RECOVERY_ERR_UNSUPPORTED = -3, /* secure boot / 加密 / 防回滚 / 调试器 */
    RECOVERY_ERR_WRITE = -4,       /* otadata 擦除或写入失败 */
    RECOVERY_ERR_VERIFY = -5,      /* 写入后回读 / 选槽校验不一致 */
} recovery_rc_t;

/* 预检;不写 flash。detail 可为 NULL;非 NULL 时总是 NUL 结尾。 */
recovery_rc_t recovery_stock_probe(char *detail, size_t detail_len);

/* 重新预检后提交。成功后按 bootloader 规则选中的是原厂槽
 * (`esp_ota_get_boot_partition()` 只作诊断);调用方负责保存进度、刷新
 * 提示并在合适时机 esp_restart()。不做任何 app 镜像写入。 */
recovery_rc_t recovery_stock_commit(char *detail, size_t detail_len);
