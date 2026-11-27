/* mp_ota.c — 第三方固件原地蓝牙自更新引擎实现（ESP32-S3 / ESP-IDF v6.1.1）。
 *
 * 安全红线：
 *   1. 绝不写入 Bootloader(0x0)、分区表(0x8000)、NVS(0x9000)、otadata(0xF000)、
 *      phy_init(0x11000)、原厂字体区(0x480000..0x528884) 以及官方原厂 App 槽；
 *   2. 永远通过 esp_ota_get_running_partition() 动态定位当前运行第三方槽，并二次
 *      确认当前槽 project_name 不是 "paperang_e2"；
 *   3. 暂存镜像本体严格 64 KiB 对齐到 0x540000，落盘前先验证该区为全 0xFF 或已带
 *      本引擎魔数头 "MPOTASTG"，且与分区表中任何分区零重叠；
 *   4. 覆写运行槽前必须通过：长度 + 整包 SHA-256 + esp_image_verify + 芯片 ID +
 *      project_name("miaopaper_e2_e2s") + 电量门限 5 重校验；
 *   5. IRAM 自刷写函数仅调用 Mask ROM SPI Flash 函数与 IRAM 复位函数，绝不访问 Flash。
 */
#include "mp_ota.h"

#include <string.h>

#include "esp_app_desc.h"
#include "esp_attr.h"
#include "esp_cpu.h"
#include "esp_efuse.h"
#include "esp_err.h"
#include "esp_flash.h"
#include "esp_image_format.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_private/system_internal.h"
#include "esp_rom_spiflash.h"
#include "esp_secure_boot.h"
#include "esp32s3/rom/cache.h"
#include "esp32s3/rom/rtc.h"
#include "hal/wdt_hal.h"
#include "psa/crypto.h"
#include "sdkconfig.h"
#include "soc/timer_group_struct.h"

#include "mp_ble.h"

static const char *TAG = "mp_ota";

#define OTA_SECTOR_SIZE     0x1000u
#define OTA_ALIGN_64K       0x10000u
#define OTA_FLASH_MIN_SIZE  0x800000u
#define OTA_TARGET_PROJECT  "miaopaper_e2_e2s"
#define OTA_STOCK_PROJECT   "paperang_e2"
#define OTA_STAGE_MAGIC     "MPOTASTG"
#define OTA_STAGE_HDR_VER   1u

typedef struct {
    char magic[8];
    uint32_t version;
    uint32_t total_len;
    uint8_t sha256[32];
} ota_stage_hdr_t;

/* 4 KiB 读写/校验缓冲显式固定在内部 DRAM，确保关 Cache 后 100% 可访问且 4 字节对齐。 */
DRAM_ATTR static uint32_t s_ota_copy_buf[OTA_SECTOR_SIZE / sizeof(uint32_t)];
DRAM_ATTR static uint32_t s_ota_verify_buf[OTA_SECTOR_SIZE / sizeof(uint32_t)];

/* 会话状态（仅在主 app 任务访问）。 */
static bool s_active;
static bool s_verified;
static uint32_t s_total_len;
static uint32_t s_written;
static uint32_t s_buf_fill;
static uint32_t s_sector_base;
static uint32_t s_target_addr;
static uint32_t s_target_size;
static uint8_t s_expected_sha256[32];

static uint32_t align_up_4k(uint32_t n)
{
    return (n + OTA_SECTOR_SIZE - 1u) & ~(OTA_SECTOR_SIZE - 1u);
}

static bool is_ota_app_part(const esp_partition_t *p)
{
    return p && p->type == ESP_PARTITION_TYPE_APP &&
           p->subtype >= ESP_PARTITION_SUBTYPE_APP_OTA_MIN &&
           p->subtype < ESP_PARTITION_SUBTYPE_APP_OTA_MAX;
}

static bool ranges_overlap(uint64_t a0, uint64_t a1, uint64_t b0, uint64_t b1)
{
    return a0 < b1 && b0 < a1;
}

/* 确认 [MP_OTA_STAGE_HDR_ADDR, MP_OTA_STAGE_DATA_ADDR + max_len) 与分区表零重叠。 */
static bool stage_region_clear_of_partitions(uint32_t data_len, uint32_t flash_size)
{
    uint64_t st0 = MP_OTA_STAGE_HDR_ADDR;
    uint64_t st1 = (uint64_t)MP_OTA_STAGE_DATA_ADDR + align_up_4k(data_len);
    if (st1 > flash_size) {
        return false;
    }

    esp_partition_iterator_t it = esp_partition_find(
        ESP_PARTITION_TYPE_ANY, ESP_PARTITION_SUBTYPE_ANY, NULL);
    bool ok = true;
    for (; it != NULL; it = esp_partition_next(it)) {
        const esp_partition_t *p = esp_partition_get(it);
        if (!p) {
            ok = false;
            break;
        }
        if (ranges_overlap(st0, st1, p->address, (uint64_t)p->address + p->size)) {
            ok = false;
            break;
        }
    }
    esp_partition_iterator_release(it);
    return ok;
}

/* 读取当前运行分区并确认允许原地覆写（非官方槽、标准 OTA 槽、64 KiB 对齐）。 */
static const esp_partition_t *validated_running_slot(void)
{
    const esp_partition_t *run = esp_ota_get_running_partition();
    if (!is_ota_app_part(run) || run->encrypted || run->readonly) {
        return NULL;
    }
    if (run->address < 0x20000u || (run->address % OTA_ALIGN_64K) != 0 ||
        run->size < MP_OTA_MIN_IMAGE_SIZE || run->size > MP_OTA_MAX_IMAGE_SIZE) {
        return NULL;
    }

    /* 严防误把官方原厂固件槽当作自更新目标。 */
    const esp_app_desc_t *desc = esp_app_get_description();
    if (!desc || strncmp(desc->project_name, OTA_STOCK_PROJECT,
                         sizeof(desc->project_name)) == 0) {
        return NULL;
    }
    return run;
}

/* 检测另一槽是否存放着官方原厂固件（仅用于只读状态上报，不写入）。 */
static bool detect_other_slot_is_stock(const esp_partition_t *run)
{
    if (!run) {
        return false;
    }
    const esp_partition_t *other = esp_ota_get_next_update_partition(run);
    if (!is_ota_app_part(other) || other->address == run->address) {
        return false;
    }
    esp_app_desc_t desc;
    memset(&desc, 0, sizeof(desc));
    if (esp_partition_read(other, 0x20, &desc, sizeof(desc)) != ESP_OK) {
        return false;
    }
    return desc.magic_word == ESP_APP_DESC_MAGIC_WORD &&
           strncmp(desc.project_name, OTA_STOCK_PROJECT,
                   sizeof(desc.project_name)) == 0;
}

/* 检查区间 [addr, addr + len) 是否全部为擦除态 0xFF。 */
static bool flash_region_is_blank(uint32_t addr, uint32_t len)
{
    uint32_t off = 0;
    while (off < len) {
        uint32_t n = len - off;
        if (n > OTA_SECTOR_SIZE) {
            n = OTA_SECTOR_SIZE;
        }
        if (esp_flash_read(NULL, s_ota_verify_buf, addr + off, n) != ESP_OK) {
            return false;
        }
        uint32_t words = n / sizeof(uint32_t);
        for (uint32_t i = 0; i < words; i++) {
            if (s_ota_verify_buf[i] != 0xFFFFFFFFu) {
                return false;
            }
        }
        off += n;
    }
    return true;
}

/* 检查暂存区是否归本引擎使用：要么带 "MPOTASTG" 魔数头，要么完全空白(0xFF)。 */
static bool stage_region_ready(uint32_t total_len, bool full_scan)
{
    ota_stage_hdr_t hdr;
    memset(&hdr, 0, sizeof(hdr));
    if (esp_flash_read(NULL, &hdr, MP_OTA_STAGE_HDR_ADDR, sizeof(hdr)) != ESP_OK) {
        return false;
    }
    if (memcmp(hdr.magic, OTA_STAGE_MAGIC, 8) == 0 &&
        hdr.version == OTA_STAGE_HDR_VER) {
        return true;
    }
    if (!full_scan) {
        /* 快速只读检测（0x50 信息接口）：检查头扇区是否空白。 */
        return flash_region_is_blank(MP_OTA_STAGE_HDR_ADDR, OTA_SECTOR_SIZE);
    }
    if (!flash_region_is_blank(MP_OTA_STAGE_HDR_ADDR, OTA_SECTOR_SIZE)) {
        return false;
    }
    return flash_region_is_blank(MP_OTA_STAGE_DATA_ADDR, align_up_4k(total_len));
}

static bool env_supports_self_ota(uint32_t *flash_size_out)
{
    if (esp_secure_boot_enabled() || esp_efuse_is_flash_encryption_enabled()) {
        return false;
    }
    uint32_t flash_size = 0;
    if (esp_flash_get_size(NULL, &flash_size) != ESP_OK ||
        flash_size < OTA_FLASH_MIN_SIZE) {
        return false;
    }
    if (flash_size_out) {
        *flash_size_out = flash_size;
    }
    return true;
}

void mp_ota_get_info(mp_ota_info_t *out)
{
    if (!out) {
        return;
    }
    memset(out, 0, sizeof(*out));
    out->running_slot = 0xFFu;
    out->max_size = MP_OTA_MAX_IMAGE_SIZE;

    const esp_app_desc_t *desc = esp_app_get_description();
    if (desc) {
        memcpy(out->project, desc->project_name, sizeof(out->project) - 1u);
        memcpy(out->version, desc->version, sizeof(out->version) - 1u);
        memcpy(out->app_elf_sha256, desc->app_elf_sha256,
               sizeof(out->app_elf_sha256));
    }

    uint32_t flash_size = 0;
    const esp_partition_t *run = validated_running_slot();
    if (run) {
        out->running_slot =
            (uint8_t)(run->subtype - ESP_PARTITION_SUBTYPE_APP_OTA_MIN);
        out->max_size = run->size;
        out->other_is_stock = detect_other_slot_is_stock(run) ? 1u : 0u;
    }

    if (run && env_supports_self_ota(&flash_size) &&
        stage_region_clear_of_partitions(run->size, flash_size) &&
        stage_region_ready(run->size, false)) {
        out->stage_ok = 1u;
    }
}

uint8_t mp_ota_begin(uint32_t total_len, const uint8_t sha256[32],
                     uint8_t batt_pct, bool batt_valid, bool charging)
{
    if (!sha256 || total_len < MP_OTA_MIN_IMAGE_SIZE ||
        total_len > MP_OTA_MAX_IMAGE_SIZE) {
        return MP_ST_BAD_PARAM;
    }
    if (batt_valid && !charging && batt_pct < MP_OTA_MIN_BATT_PCT) {
        ESP_LOGW(TAG, "ota rejected: low battery %u%%", (unsigned)batt_pct);
        return MP_ST_OTA_LOW_BATT;
    }
    if (esp_cpu_dbgr_is_attached()) {
        ESP_LOGW(TAG, "ota rejected: debugger attached");
        return MP_ST_FAIL;
    }

    uint32_t flash_size = 0;
    if (!env_supports_self_ota(&flash_size)) {
        ESP_LOGW(TAG, "ota rejected: unsupported flash/security config");
        return MP_ST_FAIL;
    }

    const esp_partition_t *run = validated_running_slot();
    if (!run || total_len > run->size) {
        ESP_LOGW(TAG, "ota rejected: invalid running slot or image too large");
        return MP_ST_BAD_PARAM;
    }
    if (!stage_region_clear_of_partitions(total_len, flash_size)) {
        ESP_LOGW(TAG, "ota rejected: stage region overlaps partition table");
        return MP_ST_OTA_STAGE_DIRTY;
    }
    if (!stage_region_ready(total_len, true)) {
        ESP_LOGW(TAG, "ota rejected: unexpected non-FF data in 0x530000+ stage");
        return MP_ST_OTA_STAGE_DIRTY;
    }

    /* 写入暂存元数据头，标记本区域已由阅读器 OTA 引擎接管。 */
    ota_stage_hdr_t hdr;
    memset(&hdr, 0, sizeof(hdr));
    memcpy(hdr.magic, OTA_STAGE_MAGIC, 8);
    hdr.version = OTA_STAGE_HDR_VER;
    hdr.total_len = total_len;
    memcpy(hdr.sha256, sha256, 32);

    if (esp_flash_erase_region(NULL, MP_OTA_STAGE_HDR_ADDR, OTA_SECTOR_SIZE) != ESP_OK ||
        esp_flash_write(NULL, &hdr, MP_OTA_STAGE_HDR_ADDR, sizeof(hdr)) != ESP_OK) {
        ESP_LOGE(TAG, "stage header write failed");
        return MP_ST_OTA_FLASH_ERR;
    }

    s_active = true;
    s_verified = false;
    s_total_len = total_len;
    s_written = 0;
    s_buf_fill = 0;
    s_sector_base = 0;
    s_target_addr = run->address;
    s_target_size = run->size;
    memcpy(s_expected_sha256, sha256, 32);
    memset(s_ota_copy_buf, 0xFF, sizeof(s_ota_copy_buf));

    ESP_LOGI(TAG, "ota begin: len=%u target=0x%06x(slot=%u) stage=0x%06x",
             (unsigned)total_len, (unsigned)s_target_addr,
             (unsigned)(run->subtype - ESP_PARTITION_SUBTYPE_APP_OTA_MIN),
             (unsigned)MP_OTA_STAGE_DATA_ADDR);
    return MP_ST_OK;
}

static uint8_t flush_stage_sector(void)
{
    uint32_t addr = MP_OTA_STAGE_DATA_ADDR + s_sector_base;
    if (esp_flash_erase_region(NULL, addr, OTA_SECTOR_SIZE) != ESP_OK ||
        esp_flash_write(NULL, s_ota_copy_buf, addr, OTA_SECTOR_SIZE) != ESP_OK ||
        esp_flash_read(NULL, s_ota_verify_buf, addr, OTA_SECTOR_SIZE) != ESP_OK ||
        memcmp(s_ota_copy_buf, s_ota_verify_buf, OTA_SECTOR_SIZE) != 0) {
        ESP_LOGE(TAG, "stage flash write/verify failed at 0x%06x", (unsigned)addr);
        return MP_ST_OTA_FLASH_ERR;
    }
    s_sector_base += OTA_SECTOR_SIZE;
    s_buf_fill = 0;
    memset(s_ota_copy_buf, 0xFF, sizeof(s_ota_copy_buf));
    return MP_ST_OK;
}

uint8_t mp_ota_write(uint32_t offset, const uint8_t *data, size_t len,
                     uint32_t *written_out)
{
    if (!s_active || !data || len == 0) {
        if (written_out) {
            *written_out = s_written;
        }
        return MP_ST_BAD_PARAM;
    }

    if (offset < s_written) {
        /* 允许对端重发已落盘的旧包，幂等忽略并返回当前真实水位。 */
        if (offset + (uint32_t)len <= s_written) {
            if (written_out) {
                *written_out = s_written;
            }
            return MP_ST_OK;
        }
        if (written_out) {
            *written_out = s_written;
        }
        return MP_ST_OTA_SEQ;
    }
    if (offset > s_written) {
        if (written_out) {
            *written_out = s_written;
        }
        return MP_ST_OTA_SEQ;
    }
    if (s_written + (uint32_t)len > s_total_len) {
        if (written_out) {
            *written_out = s_written;
        }
        return MP_ST_BAD_PARAM;
    }

    uint8_t *dst = (uint8_t *)s_ota_copy_buf;
    size_t pos = 0;
    while (pos < len) {
        size_t space = OTA_SECTOR_SIZE - s_buf_fill;
        size_t take = len - pos;
        if (take > space) {
            take = space;
        }
        memcpy(dst + s_buf_fill, data + pos, take);
        s_buf_fill += (uint32_t)take;
        pos += take;

        if (s_buf_fill == OTA_SECTOR_SIZE) {
            uint8_t st = flush_stage_sector();
            if (st != MP_ST_OK) {
                if (written_out) {
                    *written_out = s_written;
                }
                return st;
            }
        }
    }

    s_written += (uint32_t)len;
    if (written_out) {
        *written_out = s_written;
    }
    return MP_ST_OK;
}

/* 逐块从暂存区重算整包 SHA-256（独立于 esp_image_verify，防止任何旁路）。 */
static bool verify_staged_sha256(void)
{
    psa_hash_operation_t op = PSA_HASH_OPERATION_INIT;
    if (psa_hash_setup(&op, PSA_ALG_SHA_256) != PSA_SUCCESS) {
        return false;
    }

    uint32_t off = 0;
    bool ok = true;
    while (off < s_total_len) {
        uint32_t n = s_total_len - off;
        if (n > OTA_SECTOR_SIZE) {
            n = OTA_SECTOR_SIZE;
        }
        if (esp_flash_read(NULL, s_ota_verify_buf,
                           MP_OTA_STAGE_DATA_ADDR + off, n) != ESP_OK ||
            psa_hash_update(&op, (const uint8_t *)s_ota_verify_buf, n) != PSA_SUCCESS) {
            ok = false;
            break;
        }
        off += n;
    }

    uint8_t digest[32];
    size_t digest_len = 0;
    if (ok) {
        if (psa_hash_finish(&op, digest, sizeof(digest), &digest_len) != PSA_SUCCESS ||
            digest_len != 32u ||
            memcmp(digest, s_expected_sha256, 32u) != 0) {
            ok = false;
        }
    } else {
        (void)psa_hash_abort(&op);
    }
    return ok;
}

uint8_t mp_ota_verify_staged(uint8_t batt_pct, bool batt_valid, bool charging)
{
    if (!s_active || s_written != s_total_len) {
        ESP_LOGW(TAG, "ota verify: incomplete %u/%u",
                 (unsigned)s_written, (unsigned)s_total_len);
        return MP_ST_FAIL;
    }
    if (batt_valid && !charging && batt_pct < MP_OTA_MIN_BATT_PCT) {
        ESP_LOGW(TAG, "ota verify: low battery %u%%", (unsigned)batt_pct);
        return MP_ST_OTA_LOW_BATT;
    }

    /* 刷出最后不足 4 KiB 的尾部扇区（空余字节已填 0xFF）。 */
    if (s_buf_fill > 0) {
        uint8_t st = flush_stage_sector();
        if (st != MP_ST_OK) {
            return st;
        }
    }

    /* 校验 1：整包 SHA-256 必须与 0x51 约定哈希严格一致。 */
    if (!verify_staged_sha256()) {
        ESP_LOGW(TAG, "ota verify: whole-image SHA-256 mismatch");
        return MP_ST_OTA_SHA_MISMATCH;
    }

    /* 校验 2：调用 ESP-IDF 标准镜像验证器校验魔数/段结构/芯片 ID/内置 SHA-256。 */
    uint32_t aligned_len = align_up_4k(s_total_len);
    esp_partition_pos_t pos = {
        .offset = MP_OTA_STAGE_DATA_ADDR,
        .size = aligned_len,
    };
    esp_image_metadata_t meta;
    memset(&meta, 0, sizeof(meta));
    esp_err_t err = esp_image_verify(ESP_IMAGE_VERIFY_SILENT, &pos, &meta);
    if (err != ESP_OK || meta.image.magic != ESP_IMAGE_HEADER_MAGIC ||
        meta.image.chip_id != CONFIG_IDF_FIRMWARE_CHIP_ID ||
        !meta.image.hash_appended || meta.image_len != s_total_len) {
        ESP_LOGW(TAG, "ota verify: esp_image_verify failed err=%s chip=%u len=%u/%u",
                 esp_err_to_name(err), (unsigned)meta.image.chip_id,
                 (unsigned)meta.image_len, (unsigned)s_total_len);
        return MP_ST_OTA_BAD_IMAGE;
    }

    /* 校验 3：检查 esp_app_desc_t 魔数与 project_name，防止刷入非本阅读器镜像。 */
    esp_app_desc_t desc;
    memset(&desc, 0, sizeof(desc));
    if (esp_flash_read(NULL, &desc, MP_OTA_STAGE_DATA_ADDR + 0x20u,
                       sizeof(desc)) != ESP_OK ||
        desc.magic_word != ESP_APP_DESC_MAGIC_WORD ||
        strncmp(desc.project_name, OTA_TARGET_PROJECT,
                sizeof(desc.project_name)) != 0) {
        ESP_LOGW(TAG, "ota verify: bad app desc or project '%.32s'",
                 desc.project_name);
        return MP_ST_OTA_BAD_IMAGE;
    }

    /* 校验 4：二次确认目标槽仍是合法第三方运行槽且不越界。 */
    const esp_partition_t *run = validated_running_slot();
    if (!run || run->address != s_target_addr || aligned_len > run->size) {
        return MP_ST_FAIL;
    }

    s_verified = true;
    ESP_LOGI(TAG, "ota verify passed: project=%.32s ver=%.32s len=%u",
             desc.project_name, desc.version, (unsigned)s_total_len);
    return MP_ST_OK;
}

void mp_ota_abort(void)
{
    s_active = false;
    s_verified = false;
    s_total_len = 0;
    s_written = 0;
    s_buf_fill = 0;
    s_sector_base = 0;
}

/* 纯 IRAM 原子自刷写器：
 *   - 关中断、关全部看门狗、复位并暂停另一核、关闭 ICache/DCache；
 *   - 仅调用芯片 Mask ROM SPI Flash 函数将暂存区逐 4 KiB 扇区拷贝到运行槽并逐字读回校验；
 *   - 完成后调用常驻 IRAM 的 esp_restart_noos() 软复位。
 * 注意：本函数体内严禁出现任何日志、libc Flash 调用或 DROM 常量访问！ */
static void IRAM_ATTR __attribute__((noinline, noreturn))
ota_iram_copy_and_reboot(uint32_t src_addr, uint32_t dst_addr,
                         uint32_t aligned_len)
{
    /* 1) 屏蔽本核所有中断。 */
    esp_cpu_intr_disable(0xFFFFFFFFu);

    /* 2) 关闭 TG0/TG1/RTC 看门狗，避免 4~6 秒连续擦写触发 300 ms 中断看门狗复位。
     *    逐字段赋值避免编译器生成位于 DROM 的结构体初始化常量。 */
    wdt_hal_context_t wdt0;
    wdt0.inst = WDT_MWDT0;
    wdt0.mwdt_dev = &TIMERG0;
    wdt_hal_write_protect_disable(&wdt0);
    wdt_hal_disable(&wdt0);
    wdt_hal_write_protect_enable(&wdt0);

    wdt_hal_context_t wdt1;
    wdt1.inst = WDT_MWDT1;
    wdt1.mwdt_dev = &TIMERG1;
    wdt_hal_write_protect_disable(&wdt1);
    wdt_hal_disable(&wdt1);
    wdt_hal_write_protect_enable(&wdt1);

    wdt_hal_context_t rwdt;
    wdt_hal_init(&rwdt, WDT_RWDT, 0, false);
    wdt_hal_write_protect_disable(&rwdt);
    wdt_hal_disable(&rwdt);
    wdt_hal_write_protect_enable(&rwdt);

    /* 3) 复位并暂停另一核（先复位再 stall，防止另一核持有总线仲裁锁）。 */
    const uint32_t core_id = esp_cpu_get_core_id();
#if !CONFIG_ESP_SYSTEM_SINGLE_CORE_MODE
    const uint32_t other_core_id = (core_id == 0u) ? 1u : 0u;
    esp_rom_software_reset_cpu(other_core_id);
    esp_cpu_stall(other_core_id);
#else
    (void)core_id;
#endif

    /* 4) 关闭 ICache 与 DCache，彻底断开 Flash XIP。 */
    Cache_Disable_ICache();
    Cache_Disable_DCache();

    /* 5) 解锁 SPI Flash 写保护并逐 4 KiB 扇区拷贝 + 读回校验。 */
    (void)esp_rom_spiflash_unlock();

    for (uint32_t off = 0; off < aligned_len; off += OTA_SECTOR_SIZE) {
        int read_ok = 0;
        for (int r = 0; r < 3; r++) {
            if (esp_rom_spiflash_read(src_addr + off, s_ota_copy_buf,
                                      OTA_SECTOR_SIZE) == ESP_ROM_SPIFLASH_RESULT_OK) {
                read_ok = 1;
                break;
            }
        }
        if (!read_ok) {
            break;
        }

        int wrote_ok = 0;
        for (int attempt = 0; attempt < 3; attempt++) {
            if (esp_rom_spiflash_erase_sector((dst_addr + off) / OTA_SECTOR_SIZE) !=
                ESP_ROM_SPIFLASH_RESULT_OK) {
                continue;
            }
            if (esp_rom_spiflash_write(dst_addr + off, s_ota_copy_buf,
                                       OTA_SECTOR_SIZE) != ESP_ROM_SPIFLASH_RESULT_OK) {
                continue;
            }
            if (esp_rom_spiflash_read(dst_addr + off, s_ota_verify_buf,
                                      OTA_SECTOR_SIZE) != ESP_ROM_SPIFLASH_RESULT_OK) {
                continue;
            }
            int diff = 0;
            for (uint32_t k = 0; k < (OTA_SECTOR_SIZE / sizeof(uint32_t)); k++) {
                if (s_ota_copy_buf[k] != s_ota_verify_buf[k]) {
                    diff = 1;
                    break;
                }
            }
            if (!diff) {
                wrote_ok = 1;
                break;
            }
        }
        if (!wrote_ok) {
            break;
        }
    }

    /* 6) 复位外设并软重启；若全部成功则启动新版本，若极端硬件失败 Bootloader 自动回退原厂槽。 */
    esp_restart_noos();
}

void __attribute__((noreturn)) mp_ota_apply_and_reboot(void)
{
    uint32_t flash_size = 0;
    const esp_partition_t *run = validated_running_slot();
    if (!s_verified || !run || run->address != s_target_addr ||
        !env_supports_self_ota(&flash_size)) {
        ESP_LOGE(TAG, "ota apply aborted: invalid state");
        esp_restart();
    }

    uint32_t aligned_len = align_up_4k(s_total_len);
    if (aligned_len == 0 || aligned_len > s_target_size) {
        ESP_LOGE(TAG, "ota apply aborted: bad aligned_len=%u",
                 (unsigned)aligned_len);
        esp_restart();
    }

    /* 确保 ROM Flash 参数知晓整片 8 MiB 容量，避免访问 0x540000+ 时被 ROM 边界拦截。 */
    if (g_rom_flashchip.chip_size < flash_size) {
        (void)esp_rom_spiflash_config_param(g_rom_flashchip.device_id,
                                            flash_size, 0x10000u,
                                            OTA_SECTOR_SIZE, 0x100u, 0xFFFFu);
    }

    ESP_LOGI(TAG, "entering IRAM self-flasher: 0x%06x -> 0x%06x (%u B)",
             (unsigned)MP_OTA_STAGE_DATA_ADDR, (unsigned)s_target_addr,
             (unsigned)aligned_len);

    ota_iram_copy_and_reboot(MP_OTA_STAGE_DATA_ADDR, s_target_addr, aligned_len);
}
