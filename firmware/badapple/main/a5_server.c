/* E2S A5 恢复服务。BLE host 串行处理命令，不接收或擦写 App 镜像。 */
#include "a5_server.h"
#include "demo_config.h"
#include "bootloader_common.h"
#include "esp_efuse.h"
#include "esp_efuse_table.h"
#include "esp_flash.h"
#include "esp_flash_encrypt.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_ota_ops.h"
#include "esp_secure_boot.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "sdkconfig.h"
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#if defined(CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE) || defined(CONFIG_BOOTLOADER_APP_ANTI_ROLLBACK)
#error "test firmware stock selection requires ESP-IDF OTA without rollback or anti-rollback"
#endif

static const char *TAG = "a5";

#define DEV_VERSION  DEMO_VERSION
#define STOCK_SIZE   2026352u
#define FRAME_MAX    128u
#define RESP_MAX     1024u
#define SECTOR_BYTES 0x1000u
#define APP_ALIGN    0x10000u

/* 原厂镜像校验：只要另一个 OTA 槽是有效的 ESP-IDF app 镜像即可切回。
 * 这里不再绑定某个具体原厂版本（1.3.739 的 SHA-256），以便适用于不同批次的 E2S。 */
static bool valid_app_image(const esp_partition_t *p)
{
    uint8_t magic = 0;
    uint8_t desc[4] = { 0 };
    if (esp_partition_read(p, 0, &magic, 1) != ESP_OK || magic != 0xe9) return false;
    if (esp_partition_read(p, 0x20, desc, sizeof(desc)) != ESP_OK) return false;
    uint32_t desc_magic = (uint32_t)desc[0] | ((uint32_t)desc[1] << 8) |
                          ((uint32_t)desc[2] << 16) | ((uint32_t)desc[3] << 24);
    return desc_magic == 0xabcd5432u; /* ESP_APP_DESC_MAGIC_WORD */
}
static esp_timer_handle_t s_reboot_timer;
static bool s_handshake;
static bool s_commit_attempted;
static const esp_partition_t *s_preflight;
static uint8_t s_preflight_digest[32];
static uint8_t s_acc[FRAME_MAX];
static size_t s_acc_len;
static char s_sn[24];   /* 由蓝牙 MAC 派生，不绑定某一台设备 */
static void (*s_notify)(const uint8_t *, uint16_t);

static uint16_t le16(const uint8_t *p) { return p[0] | ((uint16_t)p[1] << 8); }
static uint32_t le32(const uint8_t *p) { return le16(p) | ((uint32_t)le16(p + 2) << 16); }
static void put_le16(uint8_t *p, uint16_t n) { p[0] = n; p[1] = n >> 8; }
static void put_le32(uint8_t *p, uint32_t n) { put_le16(p, (uint16_t)n); put_le16(p + 2, (uint16_t)(n >> 16)); }

#define FLASH_READ_MAX 512u

/* 供桌面工具做整片备份。只读，不提供任何写/擦除入口。 */
static uint32_t flash_bytes(void)
{
    static uint32_t size;
    if (!size) {
        uint32_t n = 0;
        if (esp_flash_get_size(NULL, &n) == ESP_OK) size = n;
    }
    return size;
}

static uint32_t flash_physical(void)
{
    static uint32_t size;
    if (!size) {
        uint32_t n = 0;
        if (esp_flash_get_physical_size(NULL, &n) == ESP_OK) size = n;
    }
    return size;
}

/* esp_flash_read() 按 chip->size 做边界检查，而 chip->size 来自引导头声明的容量。
 * 物理芯片更大时把上限抬到实测容量，否则 8 MiB 可能只能读前一半。 */
static uint32_t flash_read_limit(void)
{
    uint32_t configured = flash_bytes();
    uint32_t physical = flash_physical();
    if (physical && esp_flash_default_chip && esp_flash_default_chip->size < physical)
        esp_flash_default_chip->size = physical;
    return physical ? physical : configured;
}

static uint32_t crc32_calc(const uint8_t *d, size_t n)
{
    /* OEM CRC32Util.crc32_standard: reflected CRC-32 with init 896963873, not zlib's 0xffffffff. */
    uint32_t c = 896963873u ^ UINT32_MAX;
    for (size_t i = 0; i < n; i++) {
        c ^= d[i];
        for (int k = 0; k < 8; k++)
            c = (c >> 1) ^ (0xedb88320u & -(c & 1u));
    }
    return ~c;
}

static void respond(uint8_t parent, uint8_t child, uint8_t status,
                    const void *data, uint16_t len)
{
    static uint8_t frame[RESP_MAX];
    if (!s_notify || len > sizeof(frame) - 17) return;
    frame[0] = 0xa5;
    frame[1] = 1;
    put_le16(frame + 2, 8 + len);
    frame[4] = parent;
    frame[5] = child;
    frame[6] = 2;
    put_le16(frame + 7, 3 + len);
    frame[9] = status;
    put_le16(frame + 10, len);
    if (len) memcpy(frame + 12, data, len);
    uint32_t crc = crc32_calc(frame + 4, 8 + len);
    for (int i = 0; i < 4; i++) frame[12 + len + i] = crc >> (8 * i);
    frame[16 + len] = 0x5a;
    s_notify(frame, 17 + len);
}

static bool is_ota_slot(const esp_partition_t *p)
{
    return p && p->type == ESP_PARTITION_TYPE_APP &&
           p->subtype >= ESP_PARTITION_SUBTYPE_APP_OTA_MIN && p->subtype < ESP_PARTITION_SUBTYPE_APP_OTA_MAX;
}

static bool partition_fits(const esp_partition_t *p, uint32_t flash_size)
{
    return p && p->flash_chip == esp_flash_default_chip && p->erase_size == SECTOR_BYTES &&
           p->address >= CONFIG_PARTITION_TABLE_OFFSET + SECTOR_BYTES && p->address % SECTOR_BYTES == 0 &&
           p->size != 0 && p->size % SECTOR_BYTES == 0 && (uint64_t)p->address + p->size <= flash_size;
}

static bool overlaps(const esp_partition_t *a, const esp_partition_t *b)
{
    return a != b && a->flash_chip == b->flash_chip &&
           a->address < (uint64_t)b->address + b->size && b->address < (uint64_t)a->address + a->size;
}

typedef struct {
    const esp_partition_t *metadata, *target;
    unsigned count;
    unsigned sector;      /* 要擦写的 otadata 扇区（保留另一条最高序号记录） */
    uint32_t sequence;    /* 新写入的序号，映射到 target 槽 */
    uint8_t digest[32];   /* 预检时记录，切换前比对，防止期间原厂槽被改写 */
} stock_selection_t;

/* 不要求现有 otadata 两条记录一致。选一个比现有任何序号都大的序号，
 * 映射到 target 槽；写进“不是最高序号”的扇区，保留一条可回退记录。
 * 这样即使原厂 OTA 把两条 seq 写成相同值，恢复原厂仍然可用。 */
static bool next_boot_entry(const esp_partition_t *ota, unsigned count,
                            const esp_partition_t *target,
                            uint32_t *out_seq, unsigned *out_sector)
{
    esp_ota_select_entry_t entry[2];
    for (unsigned i = 0; i < 2; i++) {
        if (esp_partition_read(ota, i * SECTOR_BYTES, &entry[i], sizeof(entry[i])) != ESP_OK)
            return false;
    }
    uint32_t max_seq = 0;
    int high = -1;
    for (unsigned i = 0; i < 2; i++) {
        if (entry[i].ota_seq == 0xFFFFFFFF) continue;
        if (high < 0 || entry[i].ota_seq > entry[high].ota_seq) high = (int)i;
        if (entry[i].ota_seq > max_seq) max_seq = entry[i].ota_seq;
    }
    unsigned slot = target->subtype - ESP_PARTITION_SUBTYPE_APP_OTA_MIN;
    uint32_t seq = max_seq + 1;
    if (seq == 0 || seq == 0xFFFFFFFF) return false;
    while ((seq - 1) % count != slot) {
        seq++;
        if (seq == 0 || seq == 0xFFFFFFFF) return false;
    }
    *out_seq = seq;
    *out_sector = (high == 0) ? 1 : 0;
    return true;
}

static const esp_partition_t *verified_stock_slot(stock_selection_t *selection)
{
    uint16_t secure_version_bits;
    uint32_t configured_size, physical_size;
    if (!s_reboot_timer || s_commit_attempted || esp_secure_boot_enabled() || esp_flash_encryption_enabled() ||
        esp_efuse_read_field_blob(ESP_EFUSE_SECURE_VERSION, &secure_version_bits, 16) != ESP_OK || secure_version_bits != 0 ||
        esp_flash_get_size(NULL, &configured_size) != ESP_OK ||
        esp_flash_get_physical_size(NULL, &physical_size) != ESP_OK) return NULL;

    uint32_t flash_size = configured_size < physical_size ? configured_size : physical_size;
    const esp_partition_t *ota = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_OTA, NULL);
    if (!partition_fits(ota, flash_size) || ota->readonly || ota->encrypted || ota->size != 2 * SECTOR_BYTES) return NULL;

    const esp_partition_t *slots[16] = { 0 };
    unsigned count = 0;
    esp_partition_iterator_t it = esp_partition_find(ESP_PARTITION_TYPE_ANY, ESP_PARTITION_SUBTYPE_ANY, NULL);
    for (; it; it = esp_partition_next(it)) {
        const esp_partition_t *p = esp_partition_get(it);
        if (overlaps(p, ota) || (p != ota && p->type == ota->type && p->subtype == ota->subtype) ||
            (p->type == ESP_PARTITION_TYPE_APP && p->subtype == ESP_PARTITION_SUBTYPE_APP_OTA_MAX)) goto bad_layout;
        if (!is_ota_slot(p)) continue;
        unsigned n = p->subtype - ESP_PARTITION_SUBTYPE_APP_OTA_MIN;
        if (slots[n] || !partition_fits(p, flash_size) || p->address % APP_ALIGN != 0 || p->encrypted) goto bad_layout;
        slots[n] = p;
        count++;
    }
    if (count < 2) return NULL;
    for (unsigned n = 0; n < count; n++) if (!slots[n]) return NULL; /* ESP-IDF 按连续槽号解码 otadata */

    const esp_partition_t *run = esp_ota_get_running_partition();
    if (!is_ota_slot(run)) return NULL;

    for (unsigned n = 0; n < count; n++) {
        const esp_partition_t *p = slots[n];
        if (p == run || p->size < STOCK_SIZE) continue;
        uint32_t seq;
        unsigned sector;
        if (!next_boot_entry(ota, count, p, &seq, &sector)) continue;
        it = esp_partition_find(ESP_PARTITION_TYPE_ANY, ESP_PARTITION_SUBTYPE_ANY, NULL);
        if (!it) return NULL;
        for (; it; it = esp_partition_next(it)) {
            const esp_partition_t *other = esp_partition_get(it);
            if (overlaps(p, other) || overlaps(run, other)) goto bad_layout;
        }
        if (valid_app_image(p)) {
            uint8_t digest[32];
            if (esp_partition_get_sha256(p, digest) != ESP_OK) continue;
            if (selection) {
                *selection = (stock_selection_t){ ota, p, count, sector, seq, { 0 } };
                memcpy(selection->digest, digest, sizeof(digest));
            }
            return p;
        }
    }
    return NULL;

bad_layout:
    esp_partition_iterator_release(it);
    ESP_LOGE(TAG, "unrecognized or overlapping OTA layout");
    return NULL;
}

static void restart_cb(void *arg)
{
    (void)arg;
    esp_restart();
}

/* 直接写一条新的 otadata 记录：擦掉旧扇区，写入指向原厂槽的序号。
 * 不依赖现有两条记录是否一致；另一条最高序号记录保留作回退。 */
static esp_err_t write_boot_entry(const stock_selection_t *sel)
{
    esp_ota_select_entry_t e;
    memset(&e, 0xff, sizeof(e));
    e.ota_seq = sel->sequence;
    e.ota_state = ESP_OTA_IMG_UNDEFINED;
    e.crc = bootloader_common_ota_select_crc(&e);

    size_t off = (size_t)sel->sector * SECTOR_BYTES;
    esp_err_t err = esp_partition_erase_range(sel->metadata, off, SECTOR_BYTES);
    if (err != ESP_OK) return err;
    err = esp_partition_write(sel->metadata, off, &e, sizeof(e));
    if (err != ESP_OK) return err;
    esp_ota_select_entry_t rb;
    if (esp_partition_read(sel->metadata, off, &rb, sizeof(rb)) != ESP_OK) return ESP_FAIL;
    return memcmp(&rb, &e, sizeof(e)) == 0 ? ESP_OK : ESP_FAIL;
}

static uint8_t select_stock(void)
{
    if (s_commit_attempted) return 3;
    if (!s_preflight) return 2;
    stock_selection_t selection;
    const esp_partition_t *target = verified_stock_slot(&selection);
    if (target != s_preflight ||
        memcmp(selection.digest, s_preflight_digest, sizeof(selection.digest)) != 0) {
        s_preflight = NULL;
        return 2;
    }
    s_preflight = NULL;
    /* 这是唯一的持久写入；不标记测试应用有效，不走 factory 槽的整段擦除路径。 */
    s_commit_attempted = true;
    esp_err_t err = write_boot_entry(&selection);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "stock boot entry write failed: %s", esp_err_to_name(err));
        return 3;
    }
    if (esp_ota_get_boot_partition() != target) {
        ESP_LOGE(TAG, "stock boot selection not confirmed");
        return 3;
    }
    err = esp_timer_start_once(s_reboot_timer, 3000000);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "stock boot selected but restart timer failed: %s", esp_err_to_name(err));
        return 3;
    }
    ESP_LOGI(TAG, "stock boot selected; waiting for reboot");
    return 1;
}

static bool handshake(const uint8_t *c, uint16_t len)
{
    if (len != 19 || c[0] != 1 || le16(c + 1) != 16) return false;
    for (int i = 3; i < 19; i++) {
        if (!((c[i] >= '0' && c[i] <= '9') || (c[i] >= 'a' && c[i] <= 'f') ||
              (c[i] >= 'A' && c[i] <= 'F'))) return false;
    }
    /* 仅回显 challenge，不是鉴权；此服务不提供任何固件上传入口。 */
    static const char marker[] = "HELLO PAPERANG DEMO";
    enum { marker_len = sizeof(marker) - 1 };
    size_t sn_len = strlen(s_sn);
    uint8_t data[19 + 3 + marker_len + 3 + sizeof(s_sn) - 1];
    memcpy(data, c, 19);
    data[19] = 2;
    put_le16(data + 20, marker_len);
    memcpy(data + 22, marker, marker_len);
    size_t off = 22 + marker_len;
    data[off] = 3;
    put_le16(data + off + 1, (uint16_t)sn_len);
    memcpy(data + off + 3, s_sn, sn_len);
    respond(1, 23, 1, data, (uint16_t)(off + 3 + sn_len));
    return true;
}

void epd_diag(char *out, int n);   /* epd.c: 只读显示诊断 */
void epd_timing_diag(char *out, int n); /* epd.c: 真机时序测量结果 */
void ble_diag(char *out, int n);   /* main.c: 只读 BLE 广播/连接状态 */
void key_stats(char *out, int n);  /* key.c: 每脚按下次数/最后按住时长 */
void board_probe_diag(char *out, int n); /* board.c: GPIO34/GPIO5 原始电平 */
void playback_request_full(void);  /* playback.c: 下一帧强制全刷 */
void playback_request_timing(void); /* playback.c: 触发一次全刷+局刷测量 */

/* 往诊断串尾部追加一个“ 子串”，所有子函数自己负责空终止。 */
static void diag_append(char *buf, size_t cap, size_t *len, void (*fn)(char *, int))
{
    if (*len + 1 >= cap) return;
    buf[(*len)++] = ' ';
    fn(buf + *len, (int)(cap - *len));
    buf[cap - 1] = 0;
    *len += strlen(buf + *len);
}

static void handle_frame(const uint8_t *p, uint16_t len)
{
    uint8_t parent = p[0], child = p[1];
    if (parent == 1 && child == 23) {
        s_preflight = NULL;
        s_handshake = handshake(p + 5, len);
        if (s_handshake) return;
    } else if (s_handshake && parent == 3) {
        /* 只读整片 flash，绕过编程器与 ESP32 的 SPI 争用。
         * (3,1) content = off u32 LE + len u16 LE  -> 原始字节
         * (3,2) 空内容                              -> size u32 + JEDEC id u32 */
        switch (child) {
        case 1: {
            if (len != 6) { respond(3, 1, 2, NULL, 0); return; }
            uint32_t off = le32(p + 5);
            uint16_t n = le16(p + 9);
            uint32_t total = flash_read_limit();
            if (!n || n > FLASH_READ_MAX || !total || off >= total || n > total - off) {
                respond(3, 1, 2, NULL, 0);
                return;
            }
            static uint8_t buf[FLASH_READ_MAX];
            esp_err_t err = esp_flash_read(NULL, buf, off, n);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "flash read %#x+%u: %s", (unsigned)off, n, esp_err_to_name(err));
                respond(3, 1, 3, NULL, 0);
                return;
            }
            respond(3, 1, 1, buf, n);
            return;
        }
        case 2: {
            if (len != 0) { respond(3, 2, 2, NULL, 0); return; }
            uint8_t meta[12];
            uint32_t id = 0;
            if (esp_flash_read_id(NULL, &id) != ESP_OK) id = 0;
            put_le32(meta, flash_bytes());
            put_le32(meta + 4, flash_physical());
            put_le32(meta + 8, id);
            respond(3, 2, 1, meta, sizeof(meta));
            return;
        }
        default: break;
        }
    } else if (s_handshake && parent == 1 && !len) {
        switch (child) {
        case 2: respond(1, 2, 1, s_sn, (uint16_t)strlen(s_sn)); return;
        case 7: respond(1, 7, 1, DEV_VERSION, sizeof(DEV_VERSION) - 1); return;
        case 8: respond(1, 8, 1, "E2S", 3); return;
        case 19: respond(1, 19, select_stock(), NULL, 0); return;
        case 21: respond(1, 21, 1, "1.0.48", 6); return;
        case 240: {
            stock_selection_t sel;
            s_preflight = verified_stock_slot(&sel);
            if (s_preflight) memcpy(s_preflight_digest, sel.digest, sizeof(sel.digest));
            uint8_t route = s_preflight != NULL;
            respond(1, 240, 1, &route, sizeof(route));
            return;
        }
        case 241: {
            char diag[1024];
            epd_diag(diag, sizeof(diag));
            size_t len = strlen(diag);
            diag_append(diag, sizeof(diag), &len, ble_diag);
            diag_append(diag, sizeof(diag), &len, key_stats);
            diag_append(diag, sizeof(diag), &len, board_probe_diag);
            diag_append(diag, sizeof(diag), &len, epd_timing_diag);
            respond(1, 241, 1, diag, (uint16_t)strlen(diag));
            return;
        }
        case 242: /* 手动全刷一次，用于清残影；不影响正常帧流 */
            playback_request_full();
            respond(1, 242, 1, NULL, 0);
            return;
        case 243: /* 触发一次全刷+局刷严格计时；实测约 2 s 后读 244 */
            playback_request_timing();
            respond(1, 243, 1, NULL, 0);
            return;
        case 244: { /* 读回 (1,243) 的测量结果 */
            char tm[192];
            epd_timing_diag(tm, sizeof(tm));
            respond(1, 244, 1, tm, (uint16_t)strlen(tm));
            return;
        }
        default: break;
        }
    }
    respond(parent, child, 2, NULL, 0);
}

bool a5_server_on_ble_write(const uint8_t *data, uint16_t len)
{
    /* A5 每次只有一个请求在途。遇坏帧清会话，不扫描坏帧内容找可执行命令。 */
    if (!len || len > sizeof(s_acc) - s_acc_len) goto bad_frame;
    memcpy(s_acc + s_acc_len, data, len);
    s_acc_len += len;
    if (s_acc[0] != 0xa5 || (s_acc_len >= 2 && s_acc[1] != 1)) goto bad_frame;
    if (s_acc_len < 4) return true;
    uint16_t plen = le16(s_acc + 2);
    size_t total = (size_t)plen + 9;
    if (plen < 5 || total > sizeof(s_acc) || s_acc_len > total) goto bad_frame;
    if (s_acc_len < total) return true;
    if (s_acc[total - 1] != 0x5a || crc32_calc(s_acc + 4, plen) != le32(s_acc + 4 + plen) ||
        s_acc[6] != 1 || le16(s_acc + 7) != plen - 5) goto bad_frame;
    s_acc_len = 0;
    handle_frame(s_acc + 4, plen - 5);
    return true;

bad_frame:
    a5_server_disconnect();
    return false;
}

void a5_server_disconnect(void)
{
    s_acc_len = 0;
    s_handshake = false;
    s_preflight = NULL;
}

void a5_server_init(void (*notify)(const uint8_t *, uint16_t))
{
    s_notify = notify;
    uint8_t mac[6];
    if (esp_efuse_mac_get_default(mac) == ESP_OK) {
        snprintf(s_sn, sizeof(s_sn), "E2S-%02X%02X%02X%02X%02X%02X",
                 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    } else {
        snprintf(s_sn, sizeof(s_sn), "E2S-UNKNOWN");
    }
    const esp_timer_create_args_t args = { .callback = restart_cb, .name = "ota_reboot" };
    esp_err_t err = esp_timer_create(&args, &s_reboot_timer);
    if (err != ESP_OK) ESP_LOGE(TAG, "create restart timer: %s", esp_err_to_name(err));
}
