/* sim_hal.c — 本机全链路固件回归模拟器虚拟硬件层（HAL）实现。
 *
 * 为真实生产源码（mp_app.c / mp_store.c / mp_toc.c / mp_ota.c / recovery.c /
 * mp_ui.c / mp_page.c）提供高保真 Linux 主机运行环境：
 *   - 8 MiB 虚拟 SPI Flash + 实机分区表 + 硬件级越界写保护；
 *   - 标准 FIPS 180-4 SHA-256 与真实 ESP32-S3 二进制镜像格式验证器；
 *   - 虚拟墨水屏（带填充位越界审计）、AW2023 背光、电源/GPIO5 充电中断、
 *     独立虚拟 RTC 墙上时钟、mp_sim_rtc 跨深睡内存段、虚拟 BLE 收发队列；
 *   - setjmp/longjmp 拦截深睡与软重启，便于端到端回归断言。
 */
#include "sim_hal.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

#include "board.h"
#include "bootloader_common.h"
#include "display.h"
#include "driver/gpio.h"
#include "driver/rtc_io.h"
#include "esp32s3/rom/cache.h"
#include "esp32s3/rom/rtc.h"
#include "esp_app_desc.h"
#include "esp_attr.h"
#include "esp_cpu.h"
#include "esp_efuse.h"
#include "esp_efuse_table.h"
#include "esp_err.h"
#include "esp_flash.h"
#include "esp_image_format.h"
#include "esp_log.h"
#include "esp_newlib.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_pm.h"
#include "esp_private/system_internal.h"
#include "esp_rom_spiflash.h"
#include "esp_secure_boot.h"
#include "esp_sleep.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_vfs_fat.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "hal/wdt_hal.h"
#include "input.h"
#include "mp_backlight.h"
#include "mp_ble.h"
#include "power.h"
#include "psa/crypto.h"
#include "rc_common.h"
#include "sdkconfig.h"
#include "soc/timer_group_struct.h"
#include "storage.h"

/* ------------------------------------------------------------------ *
 * 跨深睡 RTC 段哨兵与链接器边界符号
 * ------------------------------------------------------------------ */
RTC_DATA_ATTR static uint64_t s_rtc_section_anchor;
/* 模拟 ESP32-S3 硬件 RTC_CNTL_PAD_HOLD_REG：跨深睡保持，只有 POR 或 rtc_gpio_hold_dis 清零。 */
RTC_DATA_ATTR static uint64_t s_rtc_pad_held_mask;
extern uint8_t __start_mp_sim_rtc[];
extern uint8_t __stop_mp_sim_rtc[];

/* ------------------------------------------------------------------ *
 * 标准 FIPS 180-4 SHA-256 实现
 * ------------------------------------------------------------------ */
static const uint32_t k_sha256_k[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u,
    0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
    0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u,
    0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
    0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu,
    0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
    0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u,
    0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
    0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u,
    0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
    0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u,
    0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
    0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u,
    0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
    0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
    0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u,
};

static uint32_t rotr32(uint32_t x, unsigned n)
{
    return (x >> n) | (x << (32u - n));
}

static void sha256_transform(uint32_t state[8], const uint8_t block[64])
{
    uint32_t w[64];
    for (int i = 0; i < 16; i++) {
        w[i] = ((uint32_t)block[i * 4] << 24) |
               ((uint32_t)block[i * 4 + 1] << 16) |
               ((uint32_t)block[i * 4 + 2] << 8) |
               (uint32_t)block[i * 4 + 3];
    }
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = rotr32(w[i - 15], 7) ^ rotr32(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = rotr32(w[i - 2], 17) ^ rotr32(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }

    uint32_t a = state[0], b = state[1], c = state[2], d = state[3];
    uint32_t e = state[4], f = state[5], g = state[6], h = state[7];

    for (int i = 0; i < 64; i++) {
        uint32_t s1 = rotr32(e, 6) ^ rotr32(e, 11) ^ rotr32(e, 25);
        uint32_t ch = (e & f) ^ ((~e) & g);
        uint32_t t1 = h + s1 + ch + k_sha256_k[i] + w[i];
        uint32_t s0 = rotr32(a, 2) ^ rotr32(a, 13) ^ rotr32(a, 22);
        uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        uint32_t t2 = s0 + maj;
        h = g;
        g = f;
        f = e;
        e = d + t1;
        d = c;
        c = b;
        b = a;
        a = t1 + t2;
    }

    state[0] += a;
    state[1] += b;
    state[2] += c;
    state[3] += d;
    state[4] += e;
    state[5] += f;
    state[6] += g;
    state[7] += h;
}

psa_status_t psa_hash_setup(psa_hash_operation_t *op, psa_algorithm_t alg)
{
    if (!op || alg != PSA_ALG_SHA_256) {
        return PSA_ERROR_INVALID_ARGUMENT;
    }
    op->state[0] = 0x6a09e667u;
    op->state[1] = 0xbb67ae85u;
    op->state[2] = 0x3c6ef372u;
    op->state[3] = 0xa54ff53au;
    op->state[4] = 0x510e527fu;
    op->state[5] = 0x9b05688cu;
    op->state[6] = 0x1f83d9abu;
    op->state[7] = 0x5be0cd19u;
    op->bitcount = 0;
    op->buflen = 0;
    op->active = 1;
    return PSA_SUCCESS;
}

psa_status_t psa_hash_update(psa_hash_operation_t *op,
                             const uint8_t *input, size_t len)
{
    if (!op || !op->active || (!input && len > 0)) {
        return PSA_ERROR_BAD_STATE;
    }
    op->bitcount += (uint64_t)len * 8u;
    size_t pos = 0;
    if (op->buflen > 0) {
        size_t take = 64u - op->buflen;
        if (take > len) {
            take = len;
        }
        memcpy(op->buffer + op->buflen, input, take);
        op->buflen += (uint32_t)take;
        pos += take;
        if (op->buflen == 64u) {
            sha256_transform(op->state, op->buffer);
            op->buflen = 0;
        }
    }
    while (pos + 64u <= len) {
        sha256_transform(op->state, input + pos);
        pos += 64u;
    }
    if (pos < len) {
        size_t rem = len - pos;
        memcpy(op->buffer, input + pos, rem);
        op->buflen = (uint32_t)rem;
    }
    return PSA_SUCCESS;
}

psa_status_t psa_hash_finish(psa_hash_operation_t *op,
                             uint8_t *hash, size_t hash_size, size_t *hash_length)
{
    if (!op || !op->active || !hash || hash_size < 32u || !hash_length) {
        return PSA_ERROR_INVALID_ARGUMENT;
    }
    uint8_t pad[64];
    memset(pad, 0, sizeof(pad));
    pad[0] = 0x80u;
    uint32_t pad_len = (op->buflen < 56u) ? (56u - op->buflen) : (120u - op->buflen);
    uint64_t bits = op->bitcount;
    uint8_t len_be[8];
    for (int i = 0; i < 8; i++) {
        len_be[7 - i] = (uint8_t)(bits >> (i * 8));
    }

    size_t rem_pad = pad_len;
    const uint8_t *p = pad;
    while (rem_pad > 0) {
        size_t take = 64u - op->buflen;
        if (take > rem_pad) {
            take = rem_pad;
        }
        memcpy(op->buffer + op->buflen, p, take);
        op->buflen += (uint32_t)take;
        p += take;
        rem_pad -= take;
        if (op->buflen == 64u) {
            sha256_transform(op->state, op->buffer);
            op->buflen = 0;
            p = pad + 1; /* 后续补零 */
            memset(pad, 0, sizeof(pad));
        }
    }
    memcpy(op->buffer + op->buflen, len_be, 8u);
    sha256_transform(op->state, op->buffer);

    for (int i = 0; i < 8; i++) {
        hash[i * 4] = (uint8_t)(op->state[i] >> 24);
        hash[i * 4 + 1] = (uint8_t)(op->state[i] >> 16);
        hash[i * 4 + 2] = (uint8_t)(op->state[i] >> 8);
        hash[i * 4 + 3] = (uint8_t)(op->state[i]);
    }
    *hash_length = 32u;
    op->active = 0;
    return PSA_SUCCESS;
}

psa_status_t psa_hash_abort(psa_hash_operation_t *op)
{
    if (op) {
        memset(op, 0, sizeof(*op));
    }
    return PSA_SUCCESS;
}

void sim_sha256(const void *data, size_t len, uint8_t out[32])
{
    psa_hash_operation_t op = PSA_HASH_OPERATION_INIT;
    size_t out_len = 0;
    (void)psa_hash_setup(&op, PSA_ALG_SHA_256);
    if (len > 0) {
        (void)psa_hash_update(&op, (const uint8_t *)data, len);
    }
    (void)psa_hash_finish(&op, out, 32u, &out_len);
}

/* ------------------------------------------------------------------ *
 * 全局虚拟硬件状态
 * ------------------------------------------------------------------ */
static bool s_verbose;

/* 时钟与 RTC */
static uint64_t s_now_ms;
static uint64_t s_rtc_unix_ms;

/* 系统退出捕获 */
static jmp_buf s_trap_buf;
static bool s_trap_armed;
static int s_last_trap;

/* GPIO / 唤醒 / 深睡审计 */
static int s_gpio_level[64];
static uint32_t s_wake_causes;
static uint64_t s_ext1_status;
static bool s_ok_hold_active;
static uint32_t s_ok_hold_rem_ms;
static sim_sleep_audit_t s_sleep_audit;

/* 电源 */
static power_status_t s_power;
static bool s_charge_changed;
static void (*s_charge_wake_cb)(void);

/* 背光 */
static bool s_bl_supported;
static uint8_t s_bl_level;
static bool s_bl_suspended;
static uint32_t s_bl_set_calls;
static uint32_t s_bl_suspend_calls;

/* 显示屏 */
static bool s_disp_ready;
static bool s_disp_asleep;
static uint8_t s_last_fb[MP_FB_BYTES];
static display_stats_t s_disp_stats;
static uint32_t s_disp_pad_violations;

/* 存储 */
static bool s_sd_ok;
static uint64_t s_fat_total;
static uint64_t s_fat_free;

/* 按键 */
static void (*s_input_wake_cb)(void);

/* BLE */
#define SIM_BLE_TX_CAP 2048
static bool s_ble_running;
static bool s_ble_adv;
static bool s_ble_connected;
static int s_ble_start_rc;
static uint32_t s_ble_start_calls;
static uint32_t s_ble_stop_calls;
static uint16_t s_ble_mtu;
static uint8_t s_ble_upload;
static void (*s_ble_time_cb)(const mp_time_t *t);
static void (*s_ble_conn_cb)(uint8_t connected);
static void (*s_ble_wake_cb)(void);
static sim_ble_pkt_t s_ble_tx[SIM_BLE_TX_CAP];
static size_t s_ble_tx_n;

/* Flash / 分区 / 安全配置 */
static uint8_t *s_flash;
static esp_flash_t s_flash_chip = { .size = SIM_FLASH_SIZE };
esp_flash_t *esp_flash_default_chip = &s_flash_chip;
esp_rom_spiflash_chip_t g_rom_flashchip;
timg_dev_t TIMERG0 = { .id = 0 };
timg_dev_t TIMERG1 = { .id = 1 };

static const esp_efuse_desc_t s_efuse_sec_ver_desc = { .id = 1 };
const esp_efuse_desc_t *ESP_EFUSE_SECURE_VERSION[] = { &s_efuse_sec_ver_desc, NULL };

static esp_partition_t s_parts[5];
static unsigned s_running_slot;
static esp_app_desc_t s_app_desc;
static bool s_secure_boot;
static bool s_flash_enc;
static uint16_t s_secure_version;
static bool s_dbgr_attached;

static bool s_guard_on;
static sim_flash_range_t s_guard_ranges[8];
static size_t s_guard_count;
static uint32_t s_guard_violations;
static uint32_t s_guard_last_addr;
static sim_iram_audit_t s_iram_audit;

static int s_dummy_pm_lock;
static int s_dummy_mutex;
static int s_dummy_task;

/* ------------------------------------------------------------------ *
 * 日志与错误码
 * ------------------------------------------------------------------ */
void sim_hal_set_verbose(bool on)
{
    s_verbose = on;
}

void sim_hal_log(char level, const char *tag, const char *fmt, ...)
{
    if (!s_verbose && level != 'E') {
        return;
    }
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "[%c][%s] ", level, tag ? tag : "?");
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    va_end(ap);
}

const char *esp_err_to_name(esp_err_t code)
{
    switch (code) {
    case ESP_OK: return "ESP_OK";
    case ESP_FAIL: return "ESP_FAIL";
    case ESP_ERR_NO_MEM: return "ESP_ERR_NO_MEM";
    case ESP_ERR_INVALID_ARG: return "ESP_ERR_INVALID_ARG";
    case ESP_ERR_INVALID_STATE: return "ESP_ERR_INVALID_STATE";
    case ESP_ERR_INVALID_SIZE: return "ESP_ERR_INVALID_SIZE";
    case ESP_ERR_NOT_FOUND: return "ESP_ERR_NOT_FOUND";
    case ESP_ERR_NOT_SUPPORTED: return "ESP_ERR_NOT_SUPPORTED";
    case ESP_ERR_TIMEOUT: return "ESP_ERR_TIMEOUT";
    case ESP_ERR_INVALID_CRC: return "ESP_ERR_INVALID_CRC";
    case ESP_ERR_IMAGE_INVALID: return "ESP_ERR_IMAGE_INVALID";
    default: return "ESP_ERR_UNKNOWN";
    }
}

/* ------------------------------------------------------------------ *
 * 初始化 / 销毁
 * ------------------------------------------------------------------ */
static void init_partitions(void)
{
    memset(s_parts, 0, sizeof(s_parts));

    s_parts[0].flash_chip = esp_flash_default_chip;
    s_parts[0].type = ESP_PARTITION_TYPE_DATA;
    s_parts[0].subtype = ESP_PARTITION_SUBTYPE_DATA_NVS;
    s_parts[0].address = SIM_PART_NVS_ADDR;
    s_parts[0].size = 0x6000u;
    s_parts[0].erase_size = SIM_FLASH_SECTOR;
    snprintf(s_parts[0].label, sizeof(s_parts[0].label), "nvs");

    s_parts[1].flash_chip = esp_flash_default_chip;
    s_parts[1].type = ESP_PARTITION_TYPE_DATA;
    s_parts[1].subtype = ESP_PARTITION_SUBTYPE_DATA_OTA;
    s_parts[1].address = SIM_PART_OTADATA_ADDR;
    s_parts[1].size = 0x2000u;
    s_parts[1].erase_size = SIM_FLASH_SECTOR;
    snprintf(s_parts[1].label, sizeof(s_parts[1].label), "otadata");

    s_parts[2].flash_chip = esp_flash_default_chip;
    s_parts[2].type = ESP_PARTITION_TYPE_DATA;
    s_parts[2].subtype = ESP_PARTITION_SUBTYPE_DATA_PHY;
    s_parts[2].address = SIM_PART_PHY_ADDR;
    s_parts[2].size = 0x1000u;
    s_parts[2].erase_size = SIM_FLASH_SECTOR;
    snprintf(s_parts[2].label, sizeof(s_parts[2].label), "phy_init");

    s_parts[3].flash_chip = esp_flash_default_chip;
    s_parts[3].type = ESP_PARTITION_TYPE_APP;
    s_parts[3].subtype = ESP_PARTITION_SUBTYPE_APP_OTA_0;
    s_parts[3].address = SIM_PART_OTA0_ADDR;
    s_parts[3].size = SIM_PART_OTA_SIZE;
    s_parts[3].erase_size = SIM_FLASH_SECTOR;
    snprintf(s_parts[3].label, sizeof(s_parts[3].label), "ota_0");

    s_parts[4].flash_chip = esp_flash_default_chip;
    s_parts[4].type = ESP_PARTITION_TYPE_APP;
    s_parts[4].subtype = ESP_PARTITION_SUBTYPE_APP_OTA_1;
    s_parts[4].address = SIM_PART_OTA1_ADDR;
    s_parts[4].size = SIM_PART_OTA_SIZE;
    s_parts[4].erase_size = SIM_FLASH_SECTOR;
    snprintf(s_parts[4].label, sizeof(s_parts[4].label), "ota_1");
}

void sim_hal_init(void)
{
    const char *env = getenv("MP_SIM_VERBOSE");
    s_verbose = (env && env[0] == '1');

    s_now_ms = 1000u;
    s_rtc_unix_ms = 1767225600ULL * 1000ULL;
    s_trap_armed = false;
    s_last_trap = SIM_TRAP_NONE;

    for (int i = 0; i < 64; i++) {
        s_gpio_level[i] = 1;
    }
    s_wake_causes = 0;
    s_ext1_status = 0;
    s_ok_hold_active = false;
    s_ok_hold_rem_ms = 0;
    memset(&s_sleep_audit, 0, sizeof(s_sleep_audit));

    memset(&s_power, 0, sizeof(s_power));
    s_power.mv = 4020;
    s_power.percent = 85;
    s_power.valid = true;
    s_power.charging = false;
    s_power.pin_mv = 2010;
    s_charge_changed = false;
    s_charge_wake_cb = NULL;

    s_bl_supported = true;
    s_bl_level = 0;
    s_bl_suspended = false;
    s_bl_set_calls = 0;
    s_bl_suspend_calls = 0;

    s_disp_ready = true;
    s_disp_asleep = false;
    memset(s_last_fb, 0xFF, sizeof(s_last_fb));
    memset(&s_disp_stats, 0, sizeof(s_disp_stats));
    s_disp_stats.family = 1;
    s_disp_pad_violations = 0;

    s_sd_ok = true;
    s_fat_total = 64ULL * 1024ULL * 1024ULL;
    s_fat_free = 48ULL * 1024ULL * 1024ULL;

    s_input_wake_cb = NULL;

    s_ble_running = true;
    s_ble_adv = true;
    s_ble_connected = false;
    s_ble_start_rc = 0;
    s_ble_start_calls = 0;
    s_ble_stop_calls = 0;
    s_ble_mtu = 247;
    s_ble_upload = 0;
    s_ble_time_cb = NULL;
    s_ble_conn_cb = NULL;
    s_ble_wake_cb = NULL;
    s_ble_tx_n = 0;

    if (!s_flash) {
        s_flash = (uint8_t *)malloc(SIM_FLASH_SIZE);
    }
    if (!s_flash) {
        fprintf(stderr, "sim_hal: 虚拟 8 MiB Flash 分配失败\n");
        abort();
    }
    memset(s_flash, 0xFF, SIM_FLASH_SIZE);
    s_flash_chip.size = SIM_FLASH_SIZE;
    memset(&g_rom_flashchip, 0, sizeof(g_rom_flashchip));
    g_rom_flashchip.device_id = 0x1740EFu;
    g_rom_flashchip.chip_size = 0x400000u; /* 故意设为 4 MiB，验证 mp_ota 自动扩容 */
    g_rom_flashchip.block_size = 0x10000u;
    g_rom_flashchip.sector_size = SIM_FLASH_SECTOR;
    g_rom_flashchip.page_size = 0x100u;
    g_rom_flashchip.status_mask = 0xFFFFu;

    init_partitions();
    s_running_slot = 1u; /* 默认运行在 ota_1，ota_0 留给原厂 */

    memset(&s_app_desc, 0, sizeof(s_app_desc));
    s_app_desc.magic_word = ESP_APP_DESC_MAGIC_WORD;
    snprintf(s_app_desc.project_name, sizeof(s_app_desc.project_name), "miaopaper_e2_e2s");
    snprintf(s_app_desc.version, sizeof(s_app_desc.version), "1.2.0");
    snprintf(s_app_desc.date, sizeof(s_app_desc.date), "Nov 27 2026");
    snprintf(s_app_desc.time, sizeof(s_app_desc.time), "04:00:00");
    snprintf(s_app_desc.idf_ver, sizeof(s_app_desc.idf_ver), "v6.1.1");
    for (int i = 0; i < 32; i++) {
        s_app_desc.app_elf_sha256[i] = (uint8_t)(0xA0u + i);
    }

    s_secure_boot = false;
    s_flash_enc = false;
    s_secure_version = 0;
    s_dbgr_attached = false;

    s_guard_on = false;
    s_guard_count = 0;
    s_guard_violations = 0;
    s_guard_last_addr = 0;
    memset(&s_iram_audit, 0, sizeof(s_iram_audit));
    s_rtc_section_anchor = 0;
    s_rtc_pad_held_mask = 0;
}

void sim_hal_shutdown(void)
{
    free(s_flash);
    s_flash = NULL;
}

/* ------------------------------------------------------------------ *
 * 时钟与 RTC 跨深睡保持
 * ------------------------------------------------------------------ */
int64_t esp_timer_get_time(void)
{
    return (int64_t)(s_now_ms * 1000ULL);
}

uint64_t sim_hal_now_ms(void)
{
    return s_now_ms;
}

void sim_hal_set_now_ms(uint64_t ms)
{
    s_now_ms = ms;
}

void sim_hal_advance_ms(uint64_t delta_ms)
{
    s_now_ms += delta_ms;
    s_rtc_unix_ms += delta_ms;
}

void sim_hal_sleep_advance_ms(uint64_t delta_ms)
{
    s_now_ms = 50u; /* 深睡冷启动后单调计时器重头开始 */
    s_rtc_unix_ms += delta_ms;
}

uint64_t sim_hal_rtc_unix_ms(void)
{
    return s_rtc_unix_ms;
}

void sim_hal_set_rtc_unix_ms(uint64_t unix_ms)
{
    s_rtc_unix_ms = unix_ms;
}

int sim_hal_gettimeofday(struct timeval *tv, void *tz)
{
    (void)tz;
    if (!tv) {
        return -1;
    }
    tv->tv_sec = (time_t)(s_rtc_unix_ms / 1000ULL);
    tv->tv_usec = (suseconds_t)((s_rtc_unix_ms % 1000ULL) * 1000ULL);
    return 0;
}

int sim_hal_settimeofday(const struct timeval *tv, const void *tz)
{
    (void)tz;
    if (!tv) {
        return -1;
    }
    s_rtc_unix_ms = (uint64_t)tv->tv_sec * 1000ULL + (uint64_t)(tv->tv_usec / 1000);
    return 0;
}

void esp_sync_timekeeping_timers(void)
{
}

int sim_hal_rtc_save(const char *path)
{
    size_t sz = (size_t)(__stop_mp_sim_rtc - __start_mp_sim_rtc);
    FILE *f = fopen(path, "wb");
    if (!f) {
        return -1;
    }
    uint64_t hdr[2] = { s_rtc_unix_ms, (uint64_t)sz };
    if (fwrite(hdr, sizeof(hdr), 1, f) != 1 ||
        (sz > 0 && fwrite(__start_mp_sim_rtc, 1, sz, f) != sz)) {
        fclose(f);
        return -1;
    }
    return fclose(f) == 0 ? 0 : -1;
}

int sim_hal_rtc_load(const char *path)
{
    size_t sz = (size_t)(__stop_mp_sim_rtc - __start_mp_sim_rtc);
    FILE *f = fopen(path, "rb");
    if (!f) {
        return -1;
    }
    uint64_t hdr[2] = { 0, 0 };
    if (fread(hdr, sizeof(hdr), 1, f) != 1 || hdr[1] != (uint64_t)sz ||
        (sz > 0 && fread(__start_mp_sim_rtc, 1, sz, f) != sz)) {
        fclose(f);
        return -1;
    }
    fclose(f);
    s_rtc_unix_ms = hdr[0];
    return 0;
}

void sim_hal_clear_rtc_mem(void)
{
    size_t sz = (size_t)(__stop_mp_sim_rtc - __start_mp_sim_rtc);
    if (sz > 0) {
        memset(__start_mp_sim_rtc, 0, sz);
    }
    s_rtc_unix_ms = 0;
}

/* ------------------------------------------------------------------ *
 * 重启/深睡拦截与 FreeRTOS 桩
 * ------------------------------------------------------------------ */
jmp_buf *sim_hal_trap_buf(void)
{
    return &s_trap_buf;
}

void sim_hal_arm_trap(bool armed)
{
    s_trap_armed = armed;
    if (armed) {
        s_last_trap = SIM_TRAP_NONE;
    }
}

int sim_hal_last_trap(void)
{
    return s_last_trap;
}

static void __attribute__((noreturn)) trigger_trap(int code)
{
    s_last_trap = code;
    if (s_trap_armed) {
        s_trap_armed = false;
        longjmp(s_trap_buf, code);
    }
    fprintf(stderr, "sim_hal: 未捕获的系统终止事件 trap=%d\n", code);
    abort();
}

void __attribute__((noreturn)) esp_restart(void)
{
    trigger_trap(SIM_TRAP_RESTART);
}

void __attribute__((noreturn)) esp_restart_noos(void)
{
    trigger_trap(SIM_TRAP_RESTART_NOOS);
}

void __attribute__((noreturn)) esp_deep_sleep_start(void)
{
    /* 高保真还原 ESP-IDF v6.1.1 sleep_modes.c ext1_wakeup_prepare()：
     * 若未显式保持 RTC_PERIPH 供电，IDF 会对 EXT1 唤醒脚强制加 RTC_CNTL_PAD_HOLD。 */
    if (!s_sleep_audit.rtc_periph_kept) {
        s_rtc_pad_held_mask |= s_sleep_audit.ext1_mask;
    }
    s_sleep_audit.rtc_pad_held_mask = s_rtc_pad_held_mask;
    s_sleep_audit.deep_sleep_entered = true;
    trigger_trap(SIM_TRAP_DEEP_SLEEP);
}

BaseType_t xPortInIsrContext(void)
{
    return pdFALSE;
}

void portYIELD_FROM_ISR(void)
{
}

TaskHandle_t xTaskGetCurrentTaskHandle(void)
{
    return (TaskHandle_t)&s_dummy_task;
}

void vTaskDelay(TickType_t xTicksToDelay)
{
    uint32_t ms = (uint32_t)xTicksToDelay;
    sim_hal_advance_ms(ms);
    if (s_ok_hold_active) {
        if (ms >= s_ok_hold_rem_ms) {
            s_ok_hold_rem_ms = 0;
            s_ok_hold_active = false;
            s_gpio_level[BOARD_KEY_OK_GPIO] = 1; /* 释放按键 */
        } else {
            s_ok_hold_rem_ms -= ms;
        }
    }
}

BaseType_t xTaskNotifyGive(TaskHandle_t t)
{
    (void)t;
    return pdPASS;
}

void vTaskNotifyGiveFromISR(TaskHandle_t t, BaseType_t *hp)
{
    (void)t;
    if (hp) {
        *hp = pdFALSE;
    }
}

uint32_t ulTaskNotifyTake(BaseType_t clear, TickType_t wait)
{
    (void)clear;
    (void)wait;
    return 1u;
}

SemaphoreHandle_t xSemaphoreCreateRecursiveMutex(void)
{
    return (SemaphoreHandle_t)&s_dummy_mutex;
}

BaseType_t xSemaphoreTakeRecursive(SemaphoreHandle_t m, TickType_t wait)
{
    (void)m;
    (void)wait;
    return pdTRUE;
}

BaseType_t xSemaphoreGiveRecursive(SemaphoreHandle_t m)
{
    (void)m;
    return pdTRUE;
}

esp_err_t esp_pm_lock_create(esp_pm_lock_type_t type, int arg,
                             const char *name, esp_pm_lock_handle_t *out)
{
    (void)type;
    (void)arg;
    (void)name;
    if (out) {
        *out = (esp_pm_lock_handle_t)&s_dummy_pm_lock;
    }
    return ESP_OK;
}

esp_err_t esp_pm_lock_acquire(esp_pm_lock_handle_t h)
{
    (void)h;
    return ESP_OK;
}

esp_err_t esp_pm_lock_release(esp_pm_lock_handle_t h)
{
    (void)h;
    return ESP_OK;
}

/* ------------------------------------------------------------------ *
 * GPIO / RTC IO / 深睡唤醒
 * ------------------------------------------------------------------ */
void sim_hal_set_wakeup(uint32_t causes, uint64_t ext1_status)
{
    s_wake_causes = causes;
    s_ext1_status = ext1_status;
}

void sim_hal_set_gpio(int gpio, int level)
{
    if (gpio >= 0 && gpio < 64) {
        s_gpio_level[gpio] = level ? 1 : 0;
    }
}

void sim_hal_set_ok_hold_ms(uint32_t hold_ms)
{
    s_gpio_level[BOARD_KEY_OK_GPIO] = (hold_ms > 0) ? 0 : 1;
    s_ok_hold_rem_ms = hold_ms;
    s_ok_hold_active = (hold_ms > 0);
}

void sim_hal_force_rtc_pad_hold(uint64_t mask)
{
    s_rtc_pad_held_mask = mask;
}

uint64_t sim_hal_rtc_pad_held_mask(void)
{
    return s_rtc_pad_held_mask;
}

const sim_sleep_audit_t *sim_hal_sleep_audit(void)
{
    return &s_sleep_audit;
}

esp_err_t gpio_config(const gpio_config_t *cfg)
{
    (void)cfg;
    return ESP_OK;
}

esp_err_t gpio_reset_pin(gpio_num_t gpio_num)
{
    (void)gpio_num;
    return ESP_OK;
}

esp_err_t gpio_set_pull_mode(gpio_num_t gpio_num, gpio_pull_mode_t pull)
{
    if (gpio_num >= 0 && gpio_num < 64) {
        if (pull == GPIO_FLOATING) {
            s_sleep_audit.floated_pins_mask |= (1ULL << gpio_num);
        } else {
            s_sleep_audit.floated_pins_mask &= ~(1ULL << gpio_num);
        }
    }
    return ESP_OK;
}

esp_err_t gpio_set_direction(gpio_num_t gpio_num, gpio_mode_t mode)
{
    (void)gpio_num;
    (void)mode;
    return ESP_OK;
}

esp_err_t gpio_set_level(gpio_num_t gpio_num, uint32_t level)
{
    if (gpio_num >= 0 && gpio_num < 64) {
        s_gpio_level[gpio_num] = level ? 1 : 0;
    }
    if (gpio_num == BOARD_VBAT_EN_GPIO) {
        s_sleep_audit.vbat_en_level = level ? 1 : 0;
    } else if (gpio_num == BOARD_SD_PWR_GPIO) {
        s_sleep_audit.sd_pwr_level = level ? 1 : 0;
    }
    return ESP_OK;
}

int gpio_get_level(gpio_num_t gpio_num)
{
    if (gpio_num >= 0 && gpio_num < 64) {
        /* 若该脚仍处于 RTC_CNTL_PAD_HOLD 锁存状态，输入采样被屏蔽，读出锁存旧高电平 1。 */
        if ((s_rtc_pad_held_mask & (1ULL << gpio_num)) != 0ULL) {
            return 1;
        }
        return s_gpio_level[gpio_num];
    }
    return 1;
}

esp_err_t gpio_hold_en(gpio_num_t gpio_num)
{
    if (gpio_num == BOARD_VBAT_EN_GPIO) {
        s_sleep_audit.vbat_en_held = true;
    } else if (gpio_num == BOARD_SD_PWR_GPIO) {
        s_sleep_audit.sd_pwr_held = true;
    }
    return ESP_OK;
}

esp_err_t gpio_hold_dis(gpio_num_t gpio_num)
{
    if (gpio_num == BOARD_VBAT_EN_GPIO) {
        s_sleep_audit.vbat_en_held = false;
    } else if (gpio_num == BOARD_SD_PWR_GPIO) {
        s_sleep_audit.sd_pwr_held = false;
    }
    return ESP_OK;
}

void gpio_deep_sleep_hold_en(void)
{
    s_sleep_audit.deep_sleep_hold = true;
}

void gpio_deep_sleep_hold_dis(void)
{
    s_sleep_audit.deep_sleep_hold = false;
}

esp_err_t rtc_gpio_init(gpio_num_t gpio_num) { (void)gpio_num; return ESP_OK; }
esp_err_t rtc_gpio_deinit(gpio_num_t gpio_num) { (void)gpio_num; return ESP_OK; }
esp_err_t rtc_gpio_set_direction(gpio_num_t g, rtc_gpio_mode_t m) { (void)g; (void)m; return ESP_OK; }
esp_err_t rtc_gpio_pullup_en(gpio_num_t g) { (void)g; return ESP_OK; }
esp_err_t rtc_gpio_pulldown_dis(gpio_num_t g) { (void)g; return ESP_OK; }

esp_err_t rtc_gpio_hold_en(gpio_num_t g)
{
    if (g >= 0 && g < 64) {
        s_rtc_pad_held_mask |= (1ULL << g);
    }
    return ESP_OK;
}

esp_err_t rtc_gpio_hold_dis(gpio_num_t g)
{
    if (g >= 0 && g < 64) {
        s_rtc_pad_held_mask &= ~(1ULL << g);
    }
    return ESP_OK;
}

esp_err_t esp_sleep_disable_wakeup_source(esp_sleep_source_t source)
{
    (void)source;
    s_sleep_audit.ext1_mask = 0;
    return ESP_OK;
}

esp_err_t esp_sleep_pd_config(esp_sleep_pd_domain_t domain,
                              esp_sleep_pd_option_t option)
{
    if (domain == ESP_PD_DOMAIN_RTC_PERIPH) {
        s_sleep_audit.rtc_periph_kept = (option == ESP_PD_OPTION_ON);
    } else if (domain == ESP_PD_DOMAIN_RTC_SLOW_MEM) {
        s_sleep_audit.rtc_slow_mem_kept = (option == ESP_PD_OPTION_ON);
    } else if (domain == ESP_PD_DOMAIN_XTAL32K) {
        s_sleep_audit.xtal32k_kept = (option == ESP_PD_OPTION_ON);
    }
    return ESP_OK;
}

esp_err_t esp_sleep_enable_ext1_wakeup_io(uint64_t io_mask,
                                          esp_sleep_ext1_wakeup_mode_t mode)
{
    (void)mode;
    s_sleep_audit.ext1_mask = io_mask;
    return ESP_OK;
}

uint32_t esp_sleep_get_wakeup_causes(void)
{
    return s_wake_causes;
}

uint64_t esp_sleep_get_ext1_wakeup_status(void)
{
    return s_ext1_status;
}

/* ------------------------------------------------------------------ *
 * 电源 / 充电检测
 * ------------------------------------------------------------------ */
void sim_hal_set_battery(uint8_t pct, uint16_t mv, bool valid, bool charging)
{
    s_power.percent = pct;
    s_power.mv = mv;
    s_power.pin_mv = mv / 2u;
    s_power.valid = valid;
    s_power.charging = charging;
    s_gpio_level[BOARD_CHARGE_DET_GPIO] = charging ? 0 : 1;
}

void sim_hal_set_charging(bool charging, bool fire_irq)
{
    if (s_power.charging != charging) {
        s_power.charging = charging;
        s_gpio_level[BOARD_CHARGE_DET_GPIO] = charging ? 0 : 1;
        s_charge_changed = true;
        if (fire_irq && s_charge_wake_cb) {
            s_charge_wake_cb();
        }
    }
}

int power_init(void) { return 0; }
void power_sample(void) {}
const power_status_t *power_get(void) { return &s_power; }
void power_tick(void) {}

void power_set_charge_wake_cb(void (*cb)(void))
{
    s_charge_wake_cb = cb;
}

bool power_charge_poll(bool *changed)
{
    if (changed) {
        *changed = s_charge_changed;
    }
    s_charge_changed = false;
    return s_power.charging;
}

bool power_low_warn(void) { return s_power.valid && !s_power.charging && s_power.percent < 5; }
bool power_low_critical(void) { return s_power.valid && !s_power.charging && s_power.percent < 2; }
int power_enter_deep_sleep(void) { return ESP_ERR_NOT_SUPPORTED; }

/* ------------------------------------------------------------------ *
 * 背光（AW2023 E2S / E2）
 * ------------------------------------------------------------------ */
void sim_hal_set_backlight_hw(bool supported)
{
    s_bl_supported = supported;
}

bool sim_hal_backlight_is_suspended(void)
{
    return s_bl_suspended;
}

uint8_t sim_hal_backlight_hw_level(void)
{
    return s_bl_level;
}

uint32_t sim_hal_backlight_set_calls(void)
{
    return s_bl_set_calls;
}

uint32_t sim_hal_backlight_suspend_calls(void)
{
    return s_bl_suspend_calls;
}

int mp_backlight_init(uint8_t level)
{
    if (!s_bl_supported) {
        return -1;
    }
    s_bl_level = (level > MP_BL_LEVEL_MAX) ? MP_BL_LEVEL_MAX : level;
    s_bl_suspended = (s_bl_level == 0);
    return 0;
}

bool mp_backlight_supported(void)
{
    return s_bl_supported;
}

int mp_backlight_set(uint8_t level)
{
    if (!s_bl_supported) {
        return -1;
    }
    s_bl_level = (level > MP_BL_LEVEL_MAX) ? MP_BL_LEVEL_MAX : level;
    s_bl_suspended = (s_bl_level == 0);
    s_bl_set_calls++;
    return 0;
}

void mp_backlight_suspend(void)
{
    if (!s_bl_supported) {
        return;
    }
    s_bl_suspended = true;
    s_bl_suspend_calls++;
}

void mp_backlight_deep_sleep(void)
{
    mp_backlight_suspend();
    s_sleep_audit.bl_deep_slept = true;
}

uint8_t mp_backlight_level(void)
{
    return s_bl_level;
}

/* ------------------------------------------------------------------ *
 * 显示屏（带行尾填充位自动越界检查）
 * ------------------------------------------------------------------ */
void sim_hal_set_display_ready(bool ready)
{
    s_disp_ready = ready;
}

const uint8_t *sim_hal_last_fb(void)
{
    return s_last_fb;
}

uint32_t sim_hal_display_frames(void) { return s_disp_stats.frames; }
uint32_t sim_hal_display_fulls(void) { return s_disp_stats.fulls; }
uint32_t sim_hal_display_partials(void) { return s_disp_stats.partials; }
bool sim_hal_display_asleep(void) { return s_disp_asleep; }
uint32_t sim_hal_display_padding_violations(void) { return s_disp_pad_violations; }

void sim_hal_display_get_gc(uint8_t *partials, uint16_t *idle_ms)
{
    if (partials) {
        *partials = s_disp_stats.gc_partials;
    }
    if (idle_ms) {
        *idle_ms = s_disp_stats.gc_idle_ms;
    }
}

int display_init(void) { return s_disp_ready ? 0 : -1; }
bool display_ready(void) { return s_disp_ready; }

void display_post(const uint8_t *fb, display_mode_t mode)
{
    if (!s_disp_ready || !fb || s_disp_asleep) {
        return;
    }
    memcpy(s_last_fb, fb, MP_FB_BYTES);

    /* 自动核对每一行末尾 x=250..255 的 6 个填充位必须保持白色(1)。 */
    for (int y = 0; y < MP_FB_H; y++) {
        if ((fb[y * MP_FB_ROW_BYTES + MP_FB_ROW_BYTES - 1] & 0x3Fu) != 0x3Fu) {
            s_disp_pad_violations++;
            break;
        }
    }

    s_disp_stats.frames++;
    s_disp_stats.last_ms = (uint32_t)s_now_ms;
    bool gc_due = (mode == DISPLAY_MODE_AUTO) && (s_disp_stats.gc_partials != 0) &&
                  (s_disp_stats.gc_pending ||
                   (s_disp_stats.partial_since_full + 1u) >= s_disp_stats.gc_partials);
    bool is_full = (mode == DISPLAY_MODE_FULL) || !s_disp_stats.baseline_valid ||
                   (s_disp_stats.family != 1) || gc_due;
    if (is_full) {
        s_disp_stats.fulls++;
        s_disp_stats.last_full_ms = (uint32_t)s_now_ms;
        s_disp_stats.baseline_valid = true;
        s_disp_stats.partial_since_full = 0;
        s_disp_stats.gc_pending = false;
    } else {
        s_disp_stats.partials++;
        s_disp_stats.partial_since_full++;
        if (s_disp_stats.gc_partials != 0 &&
            s_disp_stats.partial_since_full >= s_disp_stats.gc_partials) {
            s_disp_stats.gc_pending = true;
        }
    }
}

void display_flush_wait(uint32_t timeout_ms) { (void)timeout_ms; }
void display_sleep(void) { s_disp_asleep = true; }
void display_get_stats(display_stats_t *out) { if (out) *out = s_disp_stats; }
void display_request_full(void) { s_disp_stats.baseline_valid = false; }

void display_set_gc(uint8_t partials, uint16_t idle_ms)
{
    s_disp_stats.gc_partials = partials;
    s_disp_stats.gc_idle_ms = idle_ms;
    if (partials == 0) {
        s_disp_stats.gc_pending = false;
    } else if (s_disp_stats.partial_since_full >= partials) {
        s_disp_stats.gc_pending = true;
    }
}

/* ------------------------------------------------------------------ *
 * 存储 / FAT VFS / 按键桩
 * ------------------------------------------------------------------ */
void sim_hal_set_sd_ok(bool ok) { s_sd_ok = ok; }

void sim_hal_set_fat_space(uint64_t total_bytes, uint64_t free_bytes)
{
    s_fat_total = total_bytes;
    s_fat_free = free_bytes;
}

int storage_init(void) { return s_sd_ok ? 0 : -1; }
int storage_rescan(void) { return s_sd_ok ? 0 : -1; }
void storage_unmount(void) { s_sd_ok = false; }
bool storage_sd_ok(void) { return s_sd_ok; }
const char *storage_last_error(void) { return s_sd_ok ? "ok" : "sim sd offline"; }

esp_err_t esp_vfs_fat_info(const char *base_path,
                           uint64_t *out_total_bytes,
                           uint64_t *out_free_bytes)
{
    (void)base_path;
    if (!s_sd_ok) {
        return ESP_FAIL;
    }
    if (out_total_bytes) {
        *out_total_bytes = s_fat_total;
    }
    if (out_free_bytes) {
        *out_free_bytes = s_fat_free;
    }
    return ESP_OK;
}

int input_init(void) { return 0; }
void input_set_wake_cb(void (*cb)(void)) { s_input_wake_cb = cb; }
bool input_get(key_event_t *ev, uint32_t timeout_ms) { (void)ev; (void)timeout_ms; return false; }
uint32_t input_mask(void)
{
    static const int pins[5] = {
        BOARD_KEY_PREV_GPIO, BOARD_KEY_NEXT_GPIO, BOARD_KEY_MENU_GPIO,
        BOARD_KEY_OK_GPIO,   BOARD_KEY_FUNC_GPIO,
    };
    uint32_t mask = 0;
    for (int i = 0; i < 5; i++) {
        if (gpio_get_level(pins[i]) == 0) {
            mask |= 1u << pins[i];
        }
    }
    return mask;
}
uint32_t input_dropped(void) { return 0; }
uint32_t input_lost_critical(void) { return 0; }
void input_inject(key_id_t id) { (void)id; }

/* ------------------------------------------------------------------ *
 * BLE 虚拟传输层
 * ------------------------------------------------------------------ */
void sim_hal_ble_connect(bool connected, uint16_t mtu)
{
    s_ble_connected = connected;
    if (mtu >= 23u) {
        s_ble_mtu = mtu;
    }
    if (s_ble_conn_cb) {
        s_ble_conn_cb(connected ? 1u : 0u);
    }
    if (s_ble_wake_cb) {
        s_ble_wake_cb();
    }
}

void sim_hal_ble_inject_time(const mp_time_t *t)
{
    if (t && s_ble_time_cb) {
        s_ble_time_cb(t);
    }
    if (s_ble_wake_cb) {
        s_ble_wake_cb();
    }
}

void sim_hal_ble_clear_tx(void) { s_ble_tx_n = 0; }
size_t sim_hal_ble_tx_count(void) { return s_ble_tx_n; }

const sim_ble_pkt_t *sim_hal_ble_tx_at(size_t idx)
{
    return (idx < s_ble_tx_n) ? &s_ble_tx[idx] : NULL;
}

bool sim_hal_ble_adv_on(void) { return s_ble_adv; }
void sim_hal_ble_set_start_rc(int rc) { s_ble_start_rc = rc; }
uint32_t sim_hal_ble_start_calls(void) { return s_ble_start_calls; }
uint32_t sim_hal_ble_stop_calls(void) { return s_ble_stop_calls; }

int mp_ble_start(void)
{
    s_ble_start_calls++;
    if (s_ble_start_rc != 0) {
        s_ble_running = false;
        s_ble_adv = false;
        return s_ble_start_rc;
    }
    s_ble_running = true;
    return 0;
}

void mp_ble_stop(void)
{
    s_ble_stop_calls++;
    s_ble_running = false;
    s_ble_adv = false;
    s_ble_connected = false;
}

uint8_t mp_ble_running(void) { return s_ble_running ? 1u : 0u; }
void mp_ble_adv(uint8_t on) { s_ble_adv = (on != 0); }
uint8_t mp_ble_connected(void) { return s_ble_connected ? 1u : 0u; }
uint16_t mp_ble_mtu(void) { return s_ble_mtu; }

int mp_ble_notify(const void *data, uint16_t len)
{
    if (!s_ble_connected || !data || len == 0 || len > MP_BLE_PKT_MAX ||
        len > (uint16_t)(s_ble_mtu - 3u)) {
        return -1;
    }
    if (s_ble_tx_n < SIM_BLE_TX_CAP) {
        s_ble_tx[s_ble_tx_n].len = len;
        memcpy(s_ble_tx[s_ble_tx_n].data, data, len);
        s_ble_tx_n++;
    }
    return 0;
}

uint8_t mp_ble_req_get(mp_ble_req_t *out, uint32_t timeout_ms)
{
    (void)out;
    (void)timeout_ms;
    return 0;
}

void mp_ble_set_upload_active(uint8_t on) { s_ble_upload = on ? 1u : 0u; }
uint8_t mp_ble_upload_active(void) { return s_ble_upload; }

void mp_ble_stats(mp_ble_stats_t *out)
{
    if (!out) {
        return;
    }
    memset(out, 0, sizeof(*out));
    out->mtu = s_ble_mtu;
    out->connected = s_ble_connected ? 1u : 0u;
    out->adv = s_ble_adv ? 1u : 0u;
}

void mp_ble_set_time_cb(void (*cb)(const mp_time_t *t)) { s_ble_time_cb = cb; }
void mp_ble_set_conn_cb(void (*cb)(uint8_t connected)) { s_ble_conn_cb = cb; }
void mp_ble_set_wake_cb(void (*cb)(void)) { s_ble_wake_cb = cb; }
void mp_ble_publish_battery(uint8_t pct, uint16_t mv) { (void)pct; (void)mv; }
void mp_ble_publish_temp(int16_t c) { (void)c; }

/* ------------------------------------------------------------------ *
 * Flash / 分区 / 写保护红线 / ROM / WDT / Cache / 镜像验证器
 * ------------------------------------------------------------------ */
uint8_t *sim_hal_flash_raw(void) { return s_flash; }

void sim_hal_flash_set_running_slot(unsigned slot)
{
    s_running_slot = (slot == 0u) ? 0u : 1u;
}

void sim_hal_flash_set_app_desc(const char *project, const char *version,
                                const char *date, const char *time_str)
{
    s_app_desc.magic_word = ESP_APP_DESC_MAGIC_WORD;
    if (project) {
        memset(s_app_desc.project_name, 0, sizeof(s_app_desc.project_name));
        snprintf(s_app_desc.project_name, sizeof(s_app_desc.project_name), "%s", project);
    }
    if (version) {
        memset(s_app_desc.version, 0, sizeof(s_app_desc.version));
        snprintf(s_app_desc.version, sizeof(s_app_desc.version), "%s", version);
    }
    if (date) {
        memset(s_app_desc.date, 0, sizeof(s_app_desc.date));
        snprintf(s_app_desc.date, sizeof(s_app_desc.date), "%s", date);
    }
    if (time_str) {
        memset(s_app_desc.time, 0, sizeof(s_app_desc.time));
        snprintf(s_app_desc.time, sizeof(s_app_desc.time), "%s", time_str);
    }
}

void sim_hal_flash_set_otadata(unsigned sector, uint32_t seq, uint32_t state)
{
    if (sector > 1u || !s_flash) {
        return;
    }
    uint32_t addr = SIM_PART_OTADATA_ADDR + sector * SIM_FLASH_SECTOR;
    memset(s_flash + addr, 0xFF, SIM_FLASH_SECTOR);
    if (seq == 0xFFFFFFFFu) {
        return;
    }
    esp_ota_select_entry_t e;
    memset(&e, 0xFF, sizeof(e));
    e.ota_seq = seq;
    e.ota_state = state;
    e.crc = bootloader_common_ota_select_crc(&e);
    memcpy(s_flash + addr, &e, sizeof(e));
}

void sim_hal_flash_set_security(bool secure_boot, bool flash_enc,
                                uint16_t secure_version, bool dbgr)
{
    s_secure_boot = secure_boot;
    s_flash_enc = flash_enc;
    s_secure_version = secure_version;
    s_dbgr_attached = dbgr;
}

void sim_hal_flash_set_write_guard(const sim_flash_range_t *ranges, size_t count)
{
    if (count > 8u) {
        count = 8u;
    }
    if (ranges && count > 0) {
        memcpy(s_guard_ranges, ranges, count * sizeof(sim_flash_range_t));
    }
    s_guard_count = count;
    s_guard_on = true;
    s_guard_violations = 0;
    s_guard_last_addr = 0;
}

void sim_hal_flash_clear_write_guard(void)
{
    s_guard_on = false;
    s_guard_count = 0;
}

uint32_t sim_hal_flash_guard_violations(void) { return s_guard_violations; }
uint32_t sim_hal_flash_last_violation_addr(void) { return s_guard_last_addr; }
const sim_iram_audit_t *sim_hal_iram_audit(void) { return &s_iram_audit; }

static bool flash_write_allowed(uint32_t addr, uint32_t len)
{
    if (!s_guard_on) {
        return true;
    }
    uint64_t end = (uint64_t)addr + len;
    for (size_t i = 0; i < s_guard_count; i++) {
        if (addr >= s_guard_ranges[i].start && end <= s_guard_ranges[i].end) {
            return true;
        }
    }
    s_guard_violations++;
    s_guard_last_addr = addr;
    fprintf(stderr, "sim_hal: 触发 Flash 写保护红线告警! addr=0x%06x len=0x%x\n",
            (unsigned)addr, (unsigned)len);
    return false;
}

const esp_app_desc_t *esp_app_get_description(void)
{
    return &s_app_desc;
}

bool esp_secure_boot_enabled(void) { return s_secure_boot; }
bool esp_efuse_is_flash_encryption_enabled(void) { return s_flash_enc; }

esp_err_t esp_efuse_read_field_blob(const esp_efuse_desc_t *field[],
                                    void *dst, size_t dst_size_bits)
{
    (void)field;
    if (!dst || dst_size_bits < 16u) {
        return ESP_ERR_INVALID_ARG;
    }
    memcpy(dst, &s_secure_version, sizeof(s_secure_version));
    return ESP_OK;
}

bool esp_cpu_dbgr_is_attached(void) { return s_dbgr_attached; }

void esp_cpu_intr_disable(uint32_t intr_mask)
{
    if (intr_mask == 0xFFFFFFFFu) {
        s_iram_audit.intr_disabled = true;
    }
}

uint32_t esp_cpu_get_core_id(void) { return 0u; }

void esp_cpu_stall(int core_id)
{
    if (core_id >= 0 && core_id < 32) {
        s_iram_audit.cpu_stalled_mask |= (1u << (unsigned)core_id);
    }
}

void esp_rom_software_reset_cpu(int cpu_no)
{
    if (cpu_no >= 0 && cpu_no < 32) {
        s_iram_audit.cpu_reset_mask |= (1u << (unsigned)cpu_no);
    }
}

uint32_t Cache_Disable_ICache(void)
{
    s_iram_audit.icache_disabled = true;
    return 0u;
}

uint32_t Cache_Disable_DCache(void)
{
    s_iram_audit.dcache_disabled = true;
    return 0u;
}

void wdt_hal_init(wdt_hal_context_t *hal, wdt_inst_t inst,
                  uint32_t prescaler, bool enable_intr)
{
    (void)prescaler;
    (void)enable_intr;
    if (hal) {
        hal->inst = inst;
        hal->mwdt_dev = NULL;
    }
}

void wdt_hal_write_protect_disable(wdt_hal_context_t *hal) { (void)hal; }
void wdt_hal_write_protect_enable(wdt_hal_context_t *hal) { (void)hal; }

void wdt_hal_disable(wdt_hal_context_t *hal)
{
    if (hal && (unsigned)hal->inst < 3u) {
        s_iram_audit.wdt_disabled[hal->inst] = true;
    }
}

/* 与 ESP-IDF ROM esp_rom_crc32_le(UINT32_MAX, &ota_seq, 4) 字节级一致。 */
uint32_t bootloader_common_ota_select_crc(const esp_ota_select_entry_t *s)
{
    if (!s) {
        return 0u;
    }
    return ~rc_crc32_update(0u, &s->ota_seq, 4u);
}

esp_err_t esp_flash_get_size(esp_flash_t *chip, uint32_t *out_size)
{
    (void)chip;
    if (!out_size) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_size = SIM_FLASH_SIZE;
    return ESP_OK;
}

esp_err_t esp_flash_read(esp_flash_t *chip, void *buffer, uint32_t address, uint32_t length)
{
    (void)chip;
    if (!s_flash || !buffer || (uint64_t)address + length > SIM_FLASH_SIZE) {
        return ESP_ERR_INVALID_ARG;
    }
    memcpy(buffer, s_flash + address, length);
    return ESP_OK;
}

esp_err_t esp_flash_erase_region(esp_flash_t *chip, uint32_t start, uint32_t len)
{
    (void)chip;
    if (!s_flash || (start % SIM_FLASH_SECTOR) != 0 || (len % SIM_FLASH_SECTOR) != 0 ||
        (uint64_t)start + len > SIM_FLASH_SIZE) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!flash_write_allowed(start, len)) {
        return ESP_FAIL;
    }
    memset(s_flash + start, 0xFF, len);
    return ESP_OK;
}

esp_err_t esp_flash_write(esp_flash_t *chip, const void *buffer, uint32_t address, uint32_t length)
{
    (void)chip;
    if (!s_flash || !buffer || (uint64_t)address + length > SIM_FLASH_SIZE) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!flash_write_allowed(address, length)) {
        return ESP_FAIL;
    }
    /* 模拟 NOR Flash 物理特性：只能把 1 写为 0（按位与）。 */
    const uint8_t *src = (const uint8_t *)buffer;
    for (uint32_t i = 0; i < length; i++) {
        s_flash[address + i] &= src[i];
    }
    return ESP_OK;
}

esp_rom_spiflash_result_t esp_rom_spiflash_unlock(void)
{
    s_iram_audit.rom_flash_unlocked = true;
    return ESP_ROM_SPIFLASH_RESULT_OK;
}

esp_rom_spiflash_result_t esp_rom_spiflash_config_param(
    uint32_t device_id, uint32_t chip_size, uint32_t block_size,
    uint32_t sector_size, uint32_t page_size, uint32_t status_mask)
{
    g_rom_flashchip.device_id = device_id;
    g_rom_flashchip.chip_size = chip_size;
    g_rom_flashchip.block_size = block_size;
    g_rom_flashchip.sector_size = sector_size;
    g_rom_flashchip.page_size = page_size;
    g_rom_flashchip.status_mask = status_mask;
    return ESP_ROM_SPIFLASH_RESULT_OK;
}

esp_rom_spiflash_result_t esp_rom_spiflash_erase_sector(uint32_t sector_num)
{
    uint32_t addr = sector_num * SIM_FLASH_SECTOR;
    if ((uint64_t)addr + SIM_FLASH_SECTOR > g_rom_flashchip.chip_size) {
        return ESP_ROM_SPIFLASH_RESULT_ERR;
    }
    if (esp_flash_erase_region(NULL, addr, SIM_FLASH_SECTOR) != ESP_OK) {
        return ESP_ROM_SPIFLASH_RESULT_ERR;
    }
    s_iram_audit.rom_erase_count++;
    return ESP_ROM_SPIFLASH_RESULT_OK;
}

esp_rom_spiflash_result_t esp_rom_spiflash_write(uint32_t dest_addr,
                                                 const uint32_t *src,
                                                 int32_t len)
{
    if (!src || len <= 0 || (uint64_t)dest_addr + (uint32_t)len > g_rom_flashchip.chip_size) {
        return ESP_ROM_SPIFLASH_RESULT_ERR;
    }
    if (esp_flash_write(NULL, src, dest_addr, (uint32_t)len) != ESP_OK) {
        return ESP_ROM_SPIFLASH_RESULT_ERR;
    }
    s_iram_audit.rom_write_count++;
    return ESP_ROM_SPIFLASH_RESULT_OK;
}

esp_rom_spiflash_result_t esp_rom_spiflash_read(uint32_t src_addr,
                                                uint32_t *dest,
                                                int32_t len)
{
    if (!dest || len <= 0 || (uint64_t)src_addr + (uint32_t)len > g_rom_flashchip.chip_size) {
        return ESP_ROM_SPIFLASH_RESULT_ERR;
    }
    return esp_flash_read(NULL, dest, src_addr, (uint32_t)len) == ESP_OK
               ? ESP_ROM_SPIFLASH_RESULT_OK
               : ESP_ROM_SPIFLASH_RESULT_ERR;
}

/* 分区迭代器 */
struct esp_partition_iterator_opaque_ {
    esp_partition_type_t type;
    esp_partition_subtype_t subtype;
    const char *label;
    size_t idx;
};

static bool part_matches(const esp_partition_t *p, esp_partition_type_t type,
                         esp_partition_subtype_t subtype, const char *label)
{
    if (type != ESP_PARTITION_TYPE_ANY && p->type != type) {
        return false;
    }
    if (subtype != ESP_PARTITION_SUBTYPE_ANY && p->subtype != subtype) {
        return false;
    }
    if (label && strcmp(p->label, label) != 0) {
        return false;
    }
    return true;
}

esp_partition_iterator_t esp_partition_find(esp_partition_type_t type,
                                            esp_partition_subtype_t subtype,
                                            const char *label)
{
    for (size_t i = 0; i < sizeof(s_parts) / sizeof(s_parts[0]); i++) {
        if (part_matches(&s_parts[i], type, subtype, label)) {
            esp_partition_iterator_t it = (esp_partition_iterator_t)malloc(sizeof(*it));
            if (!it) {
                return NULL;
            }
            it->type = type;
            it->subtype = subtype;
            it->label = label;
            it->idx = i;
            return it;
        }
    }
    return NULL;
}

const esp_partition_t *esp_partition_find_first(esp_partition_type_t type,
                                                esp_partition_subtype_t subtype,
                                                const char *label)
{
    for (size_t i = 0; i < sizeof(s_parts) / sizeof(s_parts[0]); i++) {
        if (part_matches(&s_parts[i], type, subtype, label)) {
            return &s_parts[i];
        }
    }
    return NULL;
}

esp_partition_iterator_t esp_partition_next(esp_partition_iterator_t it)
{
    if (!it) {
        return NULL;
    }
    for (size_t i = it->idx + 1u; i < sizeof(s_parts) / sizeof(s_parts[0]); i++) {
        if (part_matches(&s_parts[i], it->type, it->subtype, it->label)) {
            it->idx = i;
            return it;
        }
    }
    free(it);
    return NULL;
}

const esp_partition_t *esp_partition_get(esp_partition_iterator_t it)
{
    return it ? &s_parts[it->idx] : NULL;
}

void esp_partition_iterator_release(esp_partition_iterator_t it)
{
    free(it);
}

esp_err_t esp_partition_read(const esp_partition_t *p, size_t off, void *dst, size_t sz)
{
    if (!p || !dst || off + sz > p->size) {
        return ESP_ERR_INVALID_ARG;
    }
    return esp_flash_read(p->flash_chip, dst, p->address + (uint32_t)off, (uint32_t)sz);
}

esp_err_t esp_partition_write(const esp_partition_t *p, size_t off, const void *src, size_t sz)
{
    if (!p || !src || off + sz > p->size) {
        return ESP_ERR_INVALID_ARG;
    }
    return esp_flash_write(p->flash_chip, src, p->address + (uint32_t)off, (uint32_t)sz);
}

esp_err_t esp_partition_erase_range(const esp_partition_t *p, size_t off, size_t sz)
{
    if (!p || off + sz > p->size) {
        return ESP_ERR_INVALID_ARG;
    }
    return esp_flash_erase_region(p->flash_chip, p->address + (uint32_t)off, (uint32_t)sz);
}

const esp_partition_t *esp_ota_get_running_partition(void)
{
    return (s_running_slot == 0u) ? &s_parts[3] : &s_parts[4];
}

const esp_partition_t *esp_ota_get_next_update_partition(const esp_partition_t *start_from)
{
    const esp_partition_t *cur = start_from ? start_from : esp_ota_get_running_partition();
    return (cur == &s_parts[3]) ? &s_parts[4] : &s_parts[3];
}

const esp_partition_t *esp_ota_get_boot_partition(void)
{
    esp_ota_select_entry_t e[2];
    int best = -1;
    for (unsigned i = 0; i < 2u; i++) {
        if (esp_partition_read(&s_parts[1], i * SIM_FLASH_SECTOR, &e[i], sizeof(e[i])) != ESP_OK) {
            continue;
        }
        bool ok = e[i].ota_seq != 0xFFFFFFFFu &&
                  e[i].ota_state != ESP_OTA_IMG_INVALID &&
                  e[i].ota_state != ESP_OTA_IMG_ABORTED &&
                  e[i].crc == bootloader_common_ota_select_crc(&e[i]);
        if (ok && (best < 0 || e[i].ota_seq > e[best].ota_seq)) {
            best = (int)i;
        }
    }
    if (best < 0) {
        return &s_parts[3];
    }
    unsigned slot = (unsigned)((e[best].ota_seq - 1u) % 2u);
    return (slot == 0u) ? &s_parts[3] : &s_parts[4];
}

/* 真实 ESP32-S3 二进制镜像格式验证器（魔数、段长度、XOR 校验和与末尾 SHA-256）。 */
esp_err_t esp_image_verify(esp_image_load_mode_t mode,
                           const esp_partition_pos_t *part,
                           esp_image_metadata_t *data)
{
    (void)mode;
    if (!part || !data || !s_flash ||
        (uint64_t)part->offset + part->size > SIM_FLASH_SIZE ||
        part->size < sizeof(esp_image_header_t) + 16u) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(data, 0, sizeof(*data));
    data->start_addr = part->offset;

    const uint8_t *base = s_flash + part->offset;
    memcpy(&data->image, base, sizeof(data->image));

    if (data->image.magic != ESP_IMAGE_HEADER_MAGIC ||
        data->image.segment_count == 0 ||
        data->image.segment_count > ESP_IMAGE_MAX_SEGMENTS) {
        return ESP_ERR_IMAGE_INVALID;
    }

    uint32_t off = sizeof(esp_image_header_t);
    uint8_t xor_sum = 0xEFu;

    for (uint8_t i = 0; i < data->image.segment_count; i++) {
        if ((uint64_t)off + sizeof(esp_image_segment_header_t) > part->size) {
            return ESP_ERR_IMAGE_INVALID;
        }
        esp_image_segment_header_t sh;
        memcpy(&sh, base + off, sizeof(sh));
        off += (uint32_t)sizeof(sh);
        if (sh.data_len == 0 || (uint64_t)off + sh.data_len > part->size) {
            return ESP_ERR_IMAGE_INVALID;
        }
        data->segments[i] = sh;
        data->segment_data[i] = part->offset + off;
        for (uint32_t k = 0; k < sh.data_len; k++) {
            xor_sum ^= base[off + k];
        }
        off += sh.data_len;
    }

    /* 校验和位于 16 字节对齐后的最后一个字节。 */
    uint32_t chk_end = (off + 16u) & ~15u;
    if (chk_end > part->size) {
        return ESP_ERR_IMAGE_INVALID;
    }
    uint8_t stored_xor = base[chk_end - 1u];
    if (stored_xor != xor_sum) {
        return ESP_ERR_IMAGE_INVALID;
    }

    uint32_t img_len = chk_end;
    if (data->image.hash_appended == 1u) {
        if ((uint64_t)chk_end + 32u > part->size) {
            return ESP_ERR_IMAGE_INVALID;
        }
        uint8_t calc[32];
        sim_sha256(base, chk_end, calc);
        if (memcmp(calc, base + chk_end, 32u) != 0) {
            return ESP_ERR_IMAGE_INVALID;
        }
        memcpy(data->image_digest, calc, 32u);
        img_len = chk_end + 32u;
    }

    data->image_len = img_len;
    return ESP_OK;
}

/* 构造带完整镜像头、esp_app_desc_t、XOR 校验和与可选 SHA-256 的测试镜像。 */
size_t sim_build_esp_image(uint8_t *out, size_t cap,
                           uint16_t chip_id,
                           const char *project_name,
                           const char *version,
                           size_t payload_bytes,
                           bool append_sha256)
{
    if (payload_bytes < sizeof(esp_app_desc_t)) {
        payload_bytes = sizeof(esp_app_desc_t);
    }
    size_t off = sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t) + payload_bytes;
    size_t chk_end = (off + 16u) & ~15u;
    size_t total = chk_end + (append_sha256 ? 32u : 0u);
    if (!out || total > cap) {
        return 0;
    }

    memset(out, 0, total);
    esp_image_header_t hdr;
    memset(&hdr, 0, sizeof(hdr));
    hdr.magic = ESP_IMAGE_HEADER_MAGIC;
    hdr.segment_count = 1u;
    hdr.spi_mode = 2u; /* DIO */
    hdr.spi_size = 3u; /* 8MB */
    hdr.entry_addr = 0x40378000u;
    hdr.wp_pin = 0xEEu;
    hdr.chip_id = (esp_chip_id_t)chip_id;
    hdr.hash_appended = append_sha256 ? 1u : 0u;
    memcpy(out, &hdr, sizeof(hdr));

    esp_image_segment_header_t sh = {
        .load_addr = 0x3C000020u,
        .data_len = (uint32_t)payload_bytes,
    };
    memcpy(out + sizeof(hdr), &sh, sizeof(sh));

    esp_app_desc_t desc;
    memset(&desc, 0, sizeof(desc));
    desc.magic_word = ESP_APP_DESC_MAGIC_WORD;
    snprintf(desc.project_name, sizeof(desc.project_name), "%s",
             project_name ? project_name : "");
    snprintf(desc.version, sizeof(desc.version), "%s", version ? version : "1.0.0");
    snprintf(desc.date, sizeof(desc.date), "Nov 27 2026");
    snprintf(desc.time, sizeof(desc.time), "04:00:00");
    snprintf(desc.idf_ver, sizeof(desc.idf_ver), "v6.1.1");
    for (int i = 0; i < 32; i++) {
        desc.app_elf_sha256[i] = (uint8_t)(0x30u + i);
    }
    uint8_t *seg = out + sizeof(hdr) + sizeof(sh);
    memcpy(seg, &desc, sizeof(desc));
    for (size_t i = sizeof(desc); i < payload_bytes; i++) {
        seg[i] = (uint8_t)((i * 37u + 13u) & 0xFFu);
    }

    uint8_t xor_sum = 0xEFu;
    for (size_t i = 0; i < payload_bytes; i++) {
        xor_sum ^= seg[i];
    }
    out[chk_end - 1u] = xor_sum;

    if (append_sha256) {
        sim_sha256(out, chk_end, out + chk_end);
    }
    return total;
}
