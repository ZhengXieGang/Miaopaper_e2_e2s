#pragma once
/* mp_ota.h — 第三方固件原地蓝牙自更新引擎（不碰官方槽/分区表/Bootloader/NVS）。
 *
 * 原理（基于实机 8 MiB Flash 布局）：
 *   - 阶段 1（正常态）：接收新固件并写入 0x530000(头)/0x540000(镜像本体, 64 KiB 对齐)
 *     空白 Flash 区，传输中任意断连/断电对当前系统与官方固件零影响；
 *   - 阶段 2（5 重预检）：整包 SHA-256 + esp_image_verify + 芯片型号 +
 *     project_name("miaopaper_e2_e2s") + 电量与槽位安全门限；
 *   - 阶段 3（纯 IRAM 原子自刷写）：关外设/中断/看门狗/另一核/Cache，仅用芯片
 *     Mask ROM SPI Flash 函数将 0x540000 逐 4 KiB 扇区覆写到当前运行第三方槽
 *     并逐字读回校验，完成后软复位直接启动新固件。
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* 暂存区固定位于原厂字体区(0x480000..0x528884)之后的 8 MiB 空白区。 */
#define MP_OTA_STAGE_HDR_ADDR   0x530000u
#define MP_OTA_STAGE_DATA_ADDR  0x540000u   /* 64 KiB 严格对齐，兼容 esp_image_verify */
#define MP_OTA_MAX_IMAGE_SIZE   0x1F0000u   /* 与实机 ota_0/ota_1 分区大小严格一致 */
#define MP_OTA_MIN_IMAGE_SIZE   0x10000u    /* 64 KiB 最小合法 app 体积 */
#define MP_OTA_MIN_BATT_PCT     20u         /* 未充电时最低允许电量百分比 */
#define MP_OTA_PROTO_VER        1u

/* 0x50 只读信息查询返回体。 */
typedef struct {
    uint8_t running_slot;       /* 0=ota_0, 1=ota_1, 0xFF=未知 */
    uint8_t stage_ok;           /* 1=暂存区与运行环境满足原地更新前提 */
    uint8_t other_is_stock;     /* 1=另一槽检测到有效官方原厂固件 */
    uint32_t max_size;          /* 允许写入的最大镜像字节数 */
    char project[32];           /* 当前运行固件 project_name */
    char version[32];           /* 当前运行固件 version */
    uint8_t app_elf_sha256[32]; /* 当前运行固件 esp_app_desc_t.app_elf_sha256 */
} mp_ota_info_t;

/* 采集当前固件与分区状态（只读，锁屏态也可安全调用）。 */
void mp_ota_get_info(mp_ota_info_t *out);

/* 启动一次固件接收会话。返回 MP_ST_OK 或 MP_ST_OTA_* / MP_ST_* 状态码。 */
uint8_t mp_ota_begin(uint32_t total_len, const uint8_t sha256[32],
                     uint8_t batt_pct, bool batt_valid, bool charging);

/* 写入一段顺序镜像数据（带显式偏移检查，每满 4 KiB 擦写并读回校验）。
 * written_out 非空时返回当前已确认接收的偏移（出错时用于指导对端重传）。 */
uint8_t mp_ota_write(uint32_t offset, const uint8_t *data, size_t len,
                     uint32_t *written_out);

/* 刷新尾部缓冲并执行 5 重完整校验（整包 SHA-256 + 镜像格式 + 芯片 ID + 项目名）。 */
uint8_t mp_ota_verify_staged(uint8_t batt_pct, bool batt_valid, bool charging);

/* 中止当前接收会话（清理 RAM 状态，对当前运行槽与官方槽零影响）。 */
void mp_ota_abort(void);

/* 仅在 mp_ota_verify_staged() 返回 MP_ST_OK 且外设已收尾后调用：
 * 进入纯 IRAM 关 Cache 覆写当前第三方运行槽并软复位（永不返回）。 */
void __attribute__((noreturn)) mp_ota_apply_and_reboot(void);
