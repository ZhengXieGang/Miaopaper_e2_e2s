/* recovery.c — 切回原厂启动槽。
 *
 * 算法来自本仓库 badapple/main/a5_server.c 的 verified_stock_slot() /
 * write_boot_entry(),按“阅读器单次恢复”裁剪:
 *  - E2S 运行时表固定是两个 OTA 槽 + 0x2000 的 otadata;
 *  - 原厂 OTA 可能把两条 otadata 写成相同 seq(本机 dump 是 43/擦除态,
 *    历史上出现过 43/43),所以不假设两条记录成对一致;
 *  - 只擦写“不持有最高**可用** seq”的那个 4 KiB 扇区,保留一条 CRC/state
 *    有效的记录;两条都不可用时没有可回退槽。
 *
 * 本文件是工程里唯一直接调用 esp_partition_erase_range/write 的地方,
 * 且目标只能是 otadata 分区。不接收、不写入任何 app 镜像。
 */
#include "recovery.h"
#include "recovery_seq.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "bootloader_common.h"
#include "esp_app_desc.h"
#include "esp_cpu.h"
#include "esp_efuse.h"
#include "esp_efuse_table.h"
#include "esp_err.h"
#include "esp_flash.h"
#include "esp_flash_partitions.h"
#include "esp_image_format.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_secure_boot.h"
#include "sdkconfig.h"

#if defined(CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE) || defined(CONFIG_BOOTLOADER_APP_ANTI_ROLLBACK)
#error "stock restore writes ESP_OTA_IMG_UNDEFINED; build without OTA rollback/anti-rollback"
#endif

static const char *TAG = "recovery";

#define SECTOR_BYTES 0x1000u
#define OTADATA_SIZE (2u * SECTOR_BYTES)
#define APP_ALIGN    0x10000u
#define MIN_APP_SIZE 0x10000u
#define STOCK_PROJECT "paperang_e2"

typedef struct {
    const esp_partition_t *otadata;
    const esp_partition_t *running;
    const esp_partition_t *target;
    uint32_t sequence;
    unsigned sector;
    uint32_t image_len;
} stock_plan_t;

static void set_detail(char *detail, size_t cap, const char *msg)
{
    if (detail && cap) {
        snprintf(detail, cap, "%s", msg);
    }
}

static bool same_part(const esp_partition_t *a, const esp_partition_t *b)
{
    return a && b && a->address == b->address && a->size == b->size &&
           a->type == b->type && a->subtype == b->subtype;
}

static bool overlaps(const esp_partition_t *a, const esp_partition_t *b)
{
    if (!a || !b || same_part(a, b)) {
        return false;
    }
    uint64_t a0 = a->address;
    uint64_t a1 = (uint64_t)a->address + a->size;
    uint64_t b0 = b->address;
    uint64_t b1 = (uint64_t)b->address + b->size;
    return a0 < b1 && b0 < a1;
}

static bool is_ota_slot(const esp_partition_t *p)
{
    return p && p->type == ESP_PARTITION_TYPE_APP &&
           p->subtype >= ESP_PARTITION_SUBTYPE_APP_OTA_MIN &&
           p->subtype < ESP_PARTITION_SUBTYPE_APP_OTA_MAX;
}

static const esp_partition_t *find_otadata(uint32_t flash_size)
{
    const esp_partition_t *ota = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_OTA, NULL);
    if (!ota || ota->size != OTADATA_SIZE || ota->encrypted || ota->readonly) {
        return NULL;
    }
    if (ota->flash_chip != esp_flash_default_chip ||
        ota->erase_size != SECTOR_BYTES || ota->size % SECTOR_BYTES != 0) {
        return NULL;
    }
    /* 不能落在 bootloader/分区表所在的前两个扇区。 */
    if (ota->address % SECTOR_BYTES != 0 ||
        ota->address < CONFIG_PARTITION_TABLE_OFFSET + SECTOR_BYTES ||
        (uint64_t)ota->address + ota->size > flash_size) {
        return NULL;
    }
    return ota;
}

/* 目标镜像必须是完整、可启动、且 project_name 为原厂的 app。
 * 要求镜像带 appended SHA-256,并由 esp_image_verify() 校验;app 构建在
 * JTAG 连接时会跳过 checksum/hash 检查,所以调试器连着时直接拒绝。 */
static recovery_rc_t validate_stock(const esp_partition_t *p, esp_app_desc_t *desc,
                                    uint32_t *image_len, char *detail, size_t cap)
{
    uint8_t magic = 0;
    if (esp_partition_read(p, 0, &magic, 1) != ESP_OK ||
        magic != ESP_IMAGE_HEADER_MAGIC) {
        set_detail(detail, cap, "原厂槽没有 app 镜像");
        return RECOVERY_ERR_NO_STOCK;
    }
    if (esp_cpu_dbgr_is_attached()) {
        set_detail(detail, cap, "调试器已连接,拒绝校验");
        return RECOVERY_ERR_UNSUPPORTED;
    }

    esp_partition_pos_t pos = { .offset = p->address, .size = p->size };
    esp_image_metadata_t meta;
    memset(&meta, 0, sizeof(meta));
    esp_err_t err = esp_image_verify(ESP_IMAGE_VERIFY_SILENT, &pos, &meta);
    if (err != ESP_OK || meta.image_len == 0 || meta.image_len > p->size) {
        ESP_LOGW(TAG, "stock image verify failed: %s", esp_err_to_name(err));
        set_detail(detail, cap, "原厂镜像校验失败");
        return RECOVERY_ERR_NO_STOCK;
    }
    if (!meta.image.hash_appended) {
        ESP_LOGW(TAG, "stock image has no appended SHA-256");
        set_detail(detail, cap, "原厂镜像缺少 SHA-256");
        return RECOVERY_ERR_NO_STOCK;
    }

    /* esp_app_desc_t 固定在 app 镜像 0x20 处(24 B 镜像头 + 8 B 段头)。 */
    if (esp_partition_read(p, 0x20, desc, sizeof(*desc)) != ESP_OK ||
        desc->magic_word != ESP_APP_DESC_MAGIC_WORD) {
        set_detail(detail, cap, "原厂镜像描述符无效");
        return RECOVERY_ERR_NO_STOCK;
    }
    if (strncmp(desc->project_name, STOCK_PROJECT, sizeof(desc->project_name)) != 0) {
        ESP_LOGW(TAG, "other slot project %.32s is not %s",
                 desc->project_name, STOCK_PROJECT);
        set_detail(detail, cap, "另一槽不是原厂固件");
        return RECOVERY_ERR_NO_STOCK;
    }

    *image_len = meta.image_len;
    return RECOVERY_OK;
}

/* 与 bootloader_common_select_otadata 的可用性判断一致:
 * seq 非 0xFFFFFFFF、CRC 对、state 不是 INVALID/ABORTED。 */
static bool ota_entry_valid(const esp_ota_select_entry_t *e)
{
    return e && e->ota_seq != 0xFFFFFFFF &&
           e->crc == bootloader_common_ota_select_crc(e) &&
           e->ota_state != ESP_OTA_IMG_INVALID &&
           e->ota_state != ESP_OTA_IMG_ABORTED;
}

/* 选一个比现有任何 seq 都大的序号,映射到 target 槽;写进“不是最高可用
 * 序号”的扇区,保留一条 CRC/state 有效的记录。 */
static bool next_boot_entry(const esp_partition_t *ota,
                            const esp_partition_t *target,
                            uint32_t *seq_out, unsigned *sector_out)
{
    esp_ota_select_entry_t entry[2];
    for (unsigned i = 0; i < 2; i++) {
        if (esp_partition_read(ota, i * SECTOR_BYTES, &entry[i],
                               sizeof(entry[i])) != ESP_OK) {
            return false;
        }
    }

    uint32_t stored[2] = { entry[0].ota_seq, entry[1].ota_seq };
    bool valid[2] = { ota_entry_valid(&entry[0]), ota_entry_valid(&entry[1]) };
    unsigned slot = (unsigned)(target->subtype - ESP_PARTITION_SUBTYPE_APP_OTA_MIN);
    return recovery_next_seq(stored, valid, slot, seq_out, sector_out);
}

/* 按二级 bootloader 的规则从两条记录里选启动槽:
 * 只考虑可用记录,取 seq 最大者,slot=(seq-1)%2。 */
static bool selected_slot(const esp_partition_t *ota,
                          unsigned *slot_out, uint32_t *seq_out)
{
    esp_ota_select_entry_t e[2];
    int best = -1;
    for (unsigned i = 0; i < 2; i++) {
        if (esp_partition_read(ota, i * SECTOR_BYTES, &e[i], sizeof(e[i])) != ESP_OK) {
            return false;
        }
        if (ota_entry_valid(&e[i]) && (best < 0 || e[i].ota_seq > e[best].ota_seq)) {
            best = (int)i;
        }
    }
    if (best < 0) {
        return false;
    }
    *seq_out = e[best].ota_seq;
    *slot_out = (unsigned)((e[best].ota_seq - 1) % 2);
    return true;
}

static recovery_rc_t build_plan(stock_plan_t *plan, char *detail, size_t cap)
{
    memset(plan, 0, sizeof(*plan));

    if (esp_secure_boot_enabled() || esp_efuse_is_flash_encryption_enabled()) {
        set_detail(detail, cap, "设备启用了安全启动或加密");
        return RECOVERY_ERR_UNSUPPORTED;
    }
    uint16_t secure_version = 0;
    if (esp_efuse_read_field_blob(ESP_EFUSE_SECURE_VERSION,
                                  &secure_version, 16) != ESP_OK ||
        secure_version != 0) {
        set_detail(detail, cap, "设备启用了防回滚");
        return RECOVERY_ERR_UNSUPPORTED;
    }

    uint32_t flash_size = 0;
    if (esp_flash_get_size(NULL, &flash_size) != ESP_OK || flash_size == 0) {
        set_detail(detail, cap, "无法读取 flash 容量");
        return RECOVERY_ERR_LAYOUT;
    }

    const esp_partition_t *ota = find_otadata(flash_size);
    if (!ota) {
        set_detail(detail, cap, "otadata 分区异常");
        return RECOVERY_ERR_LAYOUT;
    }

    /* 收集 OTA 槽并检查 otadata 不与任何分区重叠;只允许一个 data/ota。 */
    const esp_partition_t *slots[2] = { NULL, NULL };
    unsigned count = 0;
    bool bad = false;
    esp_partition_iterator_t it = esp_partition_find(
        ESP_PARTITION_TYPE_ANY, ESP_PARTITION_SUBTYPE_ANY, NULL);
    for (; it != NULL; it = esp_partition_next(it)) {
        const esp_partition_t *p = esp_partition_get(it);
        if (!p) {
            bad = true;
            break;
        }
        if (!same_part(p, ota) && overlaps(p, ota)) {
            bad = true;
            break;
        }
        if (!same_part(p, ota) && p->type == ESP_PARTITION_TYPE_DATA &&
            p->subtype == ESP_PARTITION_SUBTYPE_DATA_OTA) {
            bad = true;      /* 第二个 otadata:app 与 bootloader 可能各选一个 */
            break;
        }
        if (!is_ota_slot(p)) {
            continue;
        }
        unsigned idx = (unsigned)(p->subtype - ESP_PARTITION_SUBTYPE_APP_OTA_MIN);
        if (idx >= 2 || slots[idx] || p->encrypted ||
            p->address % APP_ALIGN != 0 || p->size < MIN_APP_SIZE ||
            (uint64_t)p->address + p->size > flash_size) {
            bad = true;
            break;
        }
        slots[idx] = p;
        count++;
    }
    if (it != NULL) {
        esp_partition_iterator_release(it);
    }
    if (bad || count != 2 || !slots[0] || !slots[1]) {
        set_detail(detail, cap, "分区表或 OTA 槽异常");
        return RECOVERY_ERR_LAYOUT;
    }

    const esp_partition_t *run = esp_ota_get_running_partition();
    if (!is_ota_slot(run) ||
        (!same_part(run, slots[0]) && !same_part(run, slots[1]))) {
        set_detail(detail, cap, "当前运行槽无法识别");
        return RECOVERY_ERR_LAYOUT;
    }
    const esp_partition_t *target = same_part(run, slots[0]) ? slots[1] : slots[0];

    /* 目标槽/运行槽不得与其他分区重叠(坏分区表上写 otadata 可能伤到 app)。 */
    it = esp_partition_find(ESP_PARTITION_TYPE_ANY, ESP_PARTITION_SUBTYPE_ANY, NULL);
    if (!it) {
        set_detail(detail, cap, "分区表读取失败");
        return RECOVERY_ERR_LAYOUT;
    }
    for (; it != NULL; it = esp_partition_next(it)) {
        const esp_partition_t *p = esp_partition_get(it);
        if (!p || overlaps(p, target) || overlaps(p, run)) {
            bad = true;
            break;
        }
    }
    if (it != NULL) {
        esp_partition_iterator_release(it);
    }
    if (bad) {
        set_detail(detail, cap, "OTA 槽与其他分区重叠");
        return RECOVERY_ERR_LAYOUT;
    }

    esp_app_desc_t desc;
    uint32_t image_len = 0;
    recovery_rc_t rc = validate_stock(target, &desc, &image_len, detail, cap);
    if (rc != RECOVERY_OK) {
        return rc;
    }

    if (!next_boot_entry(ota, target, &plan->sequence, &plan->sector)) {
        set_detail(detail, cap, "otadata 序号无法递增");
        return RECOVERY_ERR_LAYOUT;
    }

    plan->otadata = ota;
    plan->running = run;
    plan->target = target;
    plan->image_len = image_len;
    ESP_LOGI(TAG, "stock slot 0x%" PRIx32 " image %" PRIu32 " B project %.32s; "
             "seq %" PRIu32 " -> slot %u (sector %u)",
             target->address, image_len, desc.project_name, plan->sequence,
             (unsigned)(target->subtype - ESP_PARTITION_SUBTYPE_APP_OTA_MIN),
             plan->sector);
    set_detail(detail, cap, "原厂槽有效");
    return RECOVERY_OK;
}

recovery_rc_t recovery_stock_probe(char *detail, size_t detail_len)
{
    stock_plan_t plan;
    return build_plan(&plan, detail, detail_len);
}

recovery_rc_t recovery_stock_commit(char *detail, size_t detail_len)
{
    stock_plan_t plan;
    recovery_rc_t rc = build_plan(&plan, detail, detail_len);
    if (rc != RECOVERY_OK) {
        return rc;
    }

    esp_ota_select_entry_t e;
    memset(&e, 0xff, sizeof(e));
    e.ota_seq = plan.sequence;
    e.ota_state = ESP_OTA_IMG_UNDEFINED;
    e.crc = bootloader_common_ota_select_crc(&e);

    size_t off = (size_t)plan.sector * SECTOR_BYTES;
    esp_err_t err = esp_partition_erase_range(plan.otadata, off, SECTOR_BYTES);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "otadata erase failed: %s", esp_err_to_name(err));
        set_detail(detail, detail_len, "otadata 擦除失败");
        return RECOVERY_ERR_WRITE;
    }
    err = esp_partition_write(plan.otadata, off, &e, sizeof(e));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "otadata write failed: %s", esp_err_to_name(err));
        set_detail(detail, detail_len, "otadata 写入失败");
        return RECOVERY_ERR_WRITE;
    }

    esp_ota_select_entry_t rb;
    if (esp_partition_read(plan.otadata, off, &rb, sizeof(rb)) != ESP_OK ||
        memcmp(&rb, &e, sizeof(e)) != 0) {
        ESP_LOGE(TAG, "otadata readback mismatch");
        set_detail(detail, detail_len, "otadata 回读不一致");
        return RECOVERY_ERR_VERIFY;
    }

    /* 用 bootloader 的选槽规则复核(不依赖 esp_ota_get_boot_partition();
     * 后者若失败，不应把已经写入成功的切换误报成失败)。 */
    unsigned slot = 0;
    uint32_t selected_seq = 0;
    if (!selected_slot(plan.otadata, &slot, &selected_seq)) {
        /* 写入记录的 32 B 回读已经一致;这里只是复核读失败。写入已提交,
         * 不能把用户留在“失败”页然后重启仍进原厂。 */
        ESP_LOGW(TAG, "otadata written but post-write selection read failed");
        set_detail(detail, detail_len, "已写入,重启后生效");
        return RECOVERY_OK;
    }
    if (slot != (unsigned)(plan.target->subtype - ESP_PARTITION_SUBTYPE_APP_OTA_MIN)) {
        ESP_LOGE(TAG, "written otadata does not select the target slot");
        set_detail(detail, detail_len, "写入后选槽校验失败");
        return RECOVERY_ERR_VERIFY;
    }
    if (!same_part(esp_ota_get_boot_partition(), plan.target)) {
        ESP_LOGW(TAG, "esp_ota_get_boot_partition() disagrees (selected seq %" PRIu32
                 " -> slot %u); using bootloader rule", selected_seq, slot);
    }

    ESP_LOGI(TAG, "boot partition -> stock 0x%" PRIx32 " (seq %" PRIu32 ")",
             plan.target->address, selected_seq);
    set_detail(detail, detail_len, "已切换到原厂槽");
    return RECOVERY_OK;
}
