/* mp_ble.c — Miaopaper 兼容的 BLE 传输层（NimBLE，ESP-IDF v6.1.1）。
 *
 * 这一层只做**传输**，不做业务语义。分工见 PORT_SPEC §10.1 与 mp_app.h:103-106：
 *
 *   本文件：GATT 服务/特征（0x221F/0x331F、0x1F10/0x1F1F、0x180F/0x2A19、
 *           0x181A/0x2A1F、0x180A/0x2A50）、广播（MPP_E2S + 0x181A Service
 *           Data）、MTU/连接参数、收包队列、通知、0xDD 对时解析、
 *           0x42-0x44 的充电脚电平回包。
 *   mp_app.c：0x10..0x44 的业务语义（BEGIN/DATA/END/ABORT、字库/锁图/
 *           设置/状态、锁屏白名单），因为只有 app 任务可以调 mp_store_*
 *           （PORT_SPEC §7.1、§10.1；platform.md §2.3 的线程模型结论）。
 *
 * 所以 0x10..0x41 在这里被**原样路由**：校验首字节范围（ble.c:195 的路由
 * 规则）、拷进队列、立即返回；真正的应答由 app 用 mp_ble_notify() 发。
 * 0x42-0x44 在参考固件里是 PC1 引脚测试、不碰存储，因此在本层就地回包。
 *
 * NimBLE 回调里只做三件事：解析首字节、`xQueueSend(..., 0)`、返回。
 * 不 notify、不碰 SD、不 vTaskDelay、不打日志（队列满/通知失败只记计数，
 * 每 256 次才 warn 一行）。"不刷屏"与"回调不阻塞"都是硬要求。
 *
 * 安全边界：
 *   - 广播名固定 "MPP_E2S"，**不含 MAC/SN/efuse**；Service Data 的 MAC6 字段
 *     恒 0（字段保留只为 18 B 结构不变）。日志也不打地址。
 *   - No_Security：不配对、不绑定、不写 NVS（CONFIG_BT_NIMBLE_NVS_PERSIST=n）。
 *   - 本文件是只读 mp_settings 的唯一允许点（PORT_SPEC §10.1 例外 2）。
 */
#include "mp_ble.h"

#include <stdatomic.h>
#include <string.h>

#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "driver/gpio.h"
#include "host/ble_att.h"
#include "host/ble_gap.h"
#include "host/ble_gatt.h"
#include "host/ble_hs.h"
#include "host/ble_hs_adv.h"
#include "host/ble_hs_id.h"
#include "host/ble_hs_mbuf.h"
#include "host/ble_sm.h"
#include "host/ble_uuid.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "os/os_mbuf.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

#include "board.h"      /* BOARD_CHARGE_DET_GPIO：0x42-0x44 的电平来源 */
#include "mp_store.h"   /* mp_settings.ble_enabled：只在启动时读一次 */
#include "rc_common.h"  /* rc_now_ms() */

/* NimBLE 的 RAM store 初始化。CONFIG_BT_NIMBLE_NVS_PERSIST=n 时它只用内存
 * 数组，不碰 NVS（sdkconfig.defaults + e2s_reader/components/ble/reader_ble.c:48
 * 的同一写法；头文件在 IDF 的私有 include 路径里，所以前置声明）。 */
void ble_store_config_init(void);

static const char *TAG = "mp_ble";

/* ------------------------------------------------------------------ *
 * 常量
 * ------------------------------------------------------------------ */
#define MP_BLE_MTU_MIN 23
#define MP_BLE_OP_MIN MP_OP_BOOK_BEGIN /* 0x10：路由下界（ble.c:195） */
#define MP_BLE_OP_MAX MP_OP_PC1_HIZ    /* 0x44：路由上界 */

/* 连接参数：上传态 7.5–10 ms / latency 0；空闲连接态 50–75 ms / latency 4，兼顾响应与省电。 */
#define MP_BLE_CONN_ITVL_MIN 6
#define MP_BLE_CONN_ITVL_MAX 8
#define MP_BLE_IDLE_ITVL_MIN 40
#define MP_BLE_IDLE_ITVL_MAX 60
#define MP_BLE_IDLE_LATENCY 4
#define MP_BLE_CONN_TIMEOUT 2000

#define MP_BLE_ADV_REFRESH_MS 60000u /* PORT_SPEC §7.6：广播 Service Data 每 60 s */
#define MP_BLE_NOTIFY_PERIOD_MS 60000u /* §7.6：0x2A19/0x2A1F 值不变时每 60 s 一次 */
#define MP_BLE_PARAM_RETRY_MS 3000u  /* 连接参数更新最多每 3 s 请求一次 */
#define MP_BLE_MTU_GOOD 100          /* 低于它就在上传前再谈一次 MTU */

/* GAP Device Name 特征值 = "ESL"（app_att.c:58，照抄；web 不读它）。
 * 广播/扫描响应名是 mp_ble.h 的 MP_BLE_NAME（"MPP_E2S"）。 */
#define MP_BLE_GAP_NAME "ESL"

/* PnP ID：app_att.c:60 的 my_PnPtrs 原样。 */
static const uint8_t s_pnp_id[7] = {0x02, 0x8a, 0x24, 0x66, 0x82, 0x01, 0x00};

/* 字节布局的编译期闸门：任何一条被改坏，0x11/0x17 的吞吐与 0x181A 的
 * 13 B 载荷都会在主机侧表现为"丢包/错位"，而不是编译错误。 */
_Static_assert(MP_BLE_DATA_MAX == MP_BLE_PKT_MAX - 1,
               "0x11/0x17 数据上限 = ATT 载荷 - opcode");
_Static_assert(MP_BLE_ADV_SVC_PAYLOAD == 13,
               "0x181A Service Data 载荷固定 13 B（ble.c:58-65）");
_Static_assert(MP_BLE_ADV_SVC_AD_LEN == 1 + 2 + MP_BLE_ADV_SVC_PAYLOAD,
               "AD 长度字节 = type(1) + UUID(2) + 载荷(13) = 16（ble.c:58-59）");
_Static_assert(sizeof(mp_ble_req_t) == 2u + MP_BLE_DATA_MAX,
               "收包队列条目必须是 op+len+data 的紧凑布局");

/* ------------------------------------------------------------------ *
 * 状态
 * ------------------------------------------------------------------ */
static bool s_started;

static QueueHandle_t s_req_q;         /* host 任务 -> app 任务（mp_ble_req_t） */
static SemaphoreHandle_t s_adv_mtx;   /* 串行化 广播 start/stop/set_data */
static SemaphoreHandle_t s_host_exit_sem; /* 等待 NimBLE host 任务安全退出 */

/* GATT 值句柄（注册后由 NimBLE 填写） */
static uint16_t s_h_main;   /* 0x331F */
static uint16_t s_h_rxtx;   /* 0x1F1F（只写，不 notify） */
static uint16_t s_h_batt;   /* 0x2A19 */
static uint16_t s_h_temp;   /* 0x2A1F */

static uint16_t s_conn = BLE_HS_CONN_HANDLE_NONE;
static volatile bool s_connected;
static volatile bool s_synced;
static volatile bool s_adv_wanted;
static volatile bool s_adv_on;
static uint8_t s_addr_type = BLE_OWN_ADDR_PUBLIC;
static uint32_t s_param_req_ms;

/* 上传会话（app 置位；只影响连接参数与统计，不代表存储状态）。 */
static volatile uint8_t s_upload;

/* 广播 Service Data 与 0x180F/0x181A 的当前值。 */
static uint8_t s_batt_pct = 100;   /* app_att.c:66 的初值 */
static uint16_t s_batt_mv;
static int16_t s_temp_c = 25;      /* E2S 无温度传感器（M §7.3 同样硬编码） */
static uint8_t s_adv_counter;
static uint32_t s_adv_data_ms;
/* NimBLE 的 svc_data_uuid16 字段是**整个 AD 载荷**（含 2 B UUID），
 * ble_hs_adv.c:474-478 原样写进 AD。所以这里前面留 2 B 放 0x181A
 * （小端 1A 18），后面 13 B 才是 ble.c:58-65 的载荷。 */
static uint8_t s_adv_svc_data[2 + MP_BLE_ADV_SVC_PAYLOAD];

/* 订阅状态（断连时 NimBLE 会清 CCCD，这里同步清零）。 */
static volatile uint8_t s_batt_sub;
static volatile uint8_t s_temp_sub;

/* 0x2A19/0x2A1F 的通知节流：值变化或每 60 s 一次（§7.6）。断开时复位，
 * 保证重连后第一次 publish 一定会发（"连接后一次"）。 */
static int16_t s_batt_notified = -1;
static uint32_t s_batt_notify_ms;
static int16_t s_temp_notified = 0x7FFF;
static uint32_t s_temp_notify_ms;

/* 计数（host 任务写，任意任务读）。 */
static atomic_uint s_rx_count;
static atomic_uint s_drop_count;
static uint16_t s_notify_errs;

/* 回调（只在 host 任务里调用；实现方只允许置标志/投递队列/唤醒通知）。 */
static void (*s_time_cb)(const mp_time_t *t);
static void (*s_conn_cb)(uint8_t connected);
static void (*s_wake_cb)(void);

static void notify_app_wake(void)
{
    void (*cb)(void) = s_wake_cb;
    if (cb != NULL) {
        cb();
    }
}

static int gap_event(struct ble_gap_event *event, void *arg);

/* ------------------------------------------------------------------ *
 * 小工具
 * ------------------------------------------------------------------ */
static void log_limited(uint16_t *counter, const char *what, int rc)
{
    (*counter)++;
    if (*counter <= 3 || (*counter & 0xFF) == 0) {
        ESP_LOGW(TAG, "%s rc=%d (n=%u)", what, rc, (unsigned)*counter);
    }
}

static void adv_payload_build(void)
{
    /* AD 载荷 = UUID(2, 小端) + MAC6(填 0) + temp2 + humi1 + batL1 + batmV2 +
     * counter1。MAC6 保持 0：广播里不出现任何身份信息（PORT_SPEC §7.2；
     * 字段保留是为了与 ble.c:58-65 的 18 B 结构一致）。 */
    memset(s_adv_svc_data, 0, sizeof s_adv_svc_data);
    s_adv_svc_data[0] = (uint8_t)(MP_BLE_SVC_ENV & 0xFF);   /* 0x1A */
    s_adv_svc_data[1] = (uint8_t)(MP_BLE_SVC_ENV >> 8);     /* 0x18 */

    uint8_t *p = s_adv_svc_data + 2;                        /* [2..14] 载荷 */
    uint16_t t = (uint16_t)s_temp_c;
    p[6] = (uint8_t)(t >> 8);        /* temp，大端（ble.c:298-300） */
    p[7] = (uint8_t)(t & 0xFF);
    p[8] = 0;                        /* humi：无传感器 */
    p[9] = s_batt_pct;               /* BatL */
    p[10] = (uint8_t)(s_batt_mv >> 8); /* BatmV，大端 */
    p[11] = (uint8_t)(s_batt_mv & 0xFF);
    p[12] = s_adv_counter;           /* 每次刷新 +1 */
}

/* 写广播数据 + 扫描响应名。调用者必须持有 s_adv_mtx。 */
static int adv_set_fields(void)
{
    struct ble_hs_adv_fields f;
    memset(&f, 0, sizeof f);
    f.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    f.svc_data_uuid16 = s_adv_svc_data;                /* UUID + 13 B 载荷 */
    f.svc_data_uuid16_len = (uint8_t)sizeof s_adv_svc_data;
    int rc = ble_gap_adv_set_fields(&f);
    if (rc != 0) {
        return rc;
    }

    /* 名字放扫描响应，与 ble.c:265 的 bls_ll_setScanRspData 一致：adv 数据
     * 只承载 flags(3 B) + 0x181A Service Data(17 B) = 20 B，扫描响应再放
     * 9 B 的名字 AD。web 用 namePrefix:'MPP' 过滤，浏览器把 adv 与 scan rsp
     * 合并后取名字，所以固定名 "MPP_E2S" 能被扫到（web:280-285）。 */
    struct ble_hs_adv_fields r;
    memset(&r, 0, sizeof r);
    r.name = (const uint8_t *)MP_BLE_NAME;
    r.name_len = (uint8_t)MP_BLE_NAME_LEN;
    r.name_is_complete = 1;
    return ble_gap_adv_rsp_set_fields(&r);
}

/* 把"想不想广播 + 当前状态"落实到控制器。可被任意任务/事件调用。 */
static void adv_apply(void)
{
    if (s_adv_mtx == NULL) {
        return;
    }
    if (xSemaphoreTake(s_adv_mtx, pdMS_TO_TICKS(200)) != pdPASS) {
        ESP_LOGD(TAG, "adv busy");
        return;
    }

    bool want = s_started && s_adv_wanted && s_synced && !s_connected;

    if (want && !s_adv_on) {
        int rc = adv_set_fields();
        if (rc == 0) {
            struct ble_gap_adv_params p;
            memset(&p, 0, sizeof p);
            p.conn_mode = BLE_GAP_CONN_MODE_UND;
            p.disc_mode = BLE_GAP_DISC_MODE_GEN;
            p.itvl_min = MP_BLE_ADV_ITVL; /* 1600 × 0.625 ms = 1.0 s */
            p.itvl_max = MP_BLE_ADV_ITVL;
            rc = ble_gap_adv_start(s_addr_type, NULL, BLE_HS_FOREVER, &p,
                                   gap_event, NULL);
            if (rc == 0 || rc == BLE_HS_EALREADY) {
                s_adv_on = true;
                ESP_LOGI(TAG, "advertising as %s", MP_BLE_NAME);
            } else {
                ESP_LOGW(TAG, "adv start rc=%d", rc);
            }
        } else {
            ESP_LOGW(TAG, "adv data rc=%d", rc);
        }
    } else if (!want && s_adv_on) {
        int rc = ble_gap_adv_stop();
        s_adv_on = false;
        if (rc != 0 && rc != BLE_HS_EALREADY) {
            ESP_LOGW(TAG, "adv stop rc=%d", rc);
        }
        ESP_LOGI(TAG, "advertising off");
    }

    xSemaphoreGive(s_adv_mtx);
}

/* 电量/温度变化时刷新广播载荷；force=0 时最多 60 s 一次（§7.6）。 */
static void adv_maybe_refresh(bool force)
{
    uint32_t now = (uint32_t)rc_now_ms();
    if (!force && s_adv_data_ms != 0 &&
        (now - s_adv_data_ms) < MP_BLE_ADV_REFRESH_MS) {
        return;
    }
    s_adv_data_ms = now;
    s_adv_counter++;
    adv_payload_build();

    if (!s_adv_on || s_adv_mtx == NULL) {
        return;                                  /* 下次 adv_apply 会重新写 */
    }
    if (xSemaphoreTake(s_adv_mtx, pdMS_TO_TICKS(200)) != pdPASS) {
        return;
    }
    int rc = adv_set_fields();
    xSemaphoreGive(s_adv_mtx);
    if (rc != 0) {
        ESP_LOGD(TAG, "adv refresh rc=%d", rc);
    }
}

static void request_fast_params(void)
{
    if (!s_connected || s_conn == BLE_HS_CONN_HANDLE_NONE) {
        return;
    }

    /* 已经在上传档位就不要再发（每次 conn param update 都会打断链路）。 */
    struct ble_gap_conn_desc d;
    if (ble_gap_conn_find(s_conn, &d) == 0 &&
        d.conn_itvl >= MP_BLE_CONN_ITVL_MIN &&
        d.conn_itvl <= MP_BLE_CONN_ITVL_MAX) {
        return;
    }

    uint32_t now = (uint32_t)rc_now_ms();
    if (s_param_req_ms != 0 && (now - s_param_req_ms) < MP_BLE_PARAM_RETRY_MS) {
        return;
    }
    s_param_req_ms = now;

    struct ble_gap_upd_params p;
    memset(&p, 0, sizeof p);
    p.itvl_min = MP_BLE_CONN_ITVL_MIN;
    p.itvl_max = MP_BLE_CONN_ITVL_MAX;
    p.latency = 0;
    p.supervision_timeout = MP_BLE_CONN_TIMEOUT;
    int rc = ble_gap_update_params(s_conn, &p);
    if (rc != 0) {
        ESP_LOGD(TAG, "conn param req rc=%d", rc);
    }
}

/* 空闲连接降频（50–75 ms, latency 4），显著降低射频保活功耗，不影响下次上传即时切回高速。 */
static void request_idle_params(void)
{
    if (!s_connected || s_conn == BLE_HS_CONN_HANDLE_NONE) {
        return;
    }
    struct ble_gap_upd_params p;
    memset(&p, 0, sizeof p);
    p.itvl_min = MP_BLE_IDLE_ITVL_MIN;
    p.itvl_max = MP_BLE_IDLE_ITVL_MAX;
    p.latency = MP_BLE_IDLE_LATENCY;
    p.supervision_timeout = MP_BLE_CONN_TIMEOUT;
    int rc = ble_gap_update_params(s_conn, &p);
    if (rc != 0) {
        ESP_LOGD(TAG, "idle param req rc=%d", rc);
    }
}

/* MTU 没谈成时再谈一次（连接事件里已经试过）。
 *
 * ATT Exchange MTU Request 只能由 GATT client 发起。sdkconfig.defaults 打开了
 * CONFIG_BT_NIMBLE_ROLE_CENTRAL + CONFIG_BT_NIMBLE_GATT_CLIENT，于是
 * MYNEWT_VAL(BLE_GATTC)==1，ble_gattc_exchange_mtu() 可用：设备在连接后主动
 * 请求 247（参考固件 ble.c:133-143 在 1.5 s / 3.0 s 各请求一次），Windows
 * 这类自己不 requestMTU 的中央也能把 MTU 谈上去，0x33 的 160 B 块才发得出去。
 * 若将来又把 GATT client 关掉，BLE_GATTC==0，下面是空分支——必须保留这个
 * 闸门，否则 ble_gattc_exchange_mtu() 只有声明、链接期会报 undefined reference
 * （ble_gattc.c 的 client 实现整个被 #if 掉）。 */
static void exchange_mtu_best_effort(void)
{
    if (!s_connected || s_conn == BLE_HS_CONN_HANDLE_NONE) {
        return;
    }
    if (ble_att_mtu(s_conn) >= MP_BLE_MTU_GOOD) {
        return;
    }
#if MYNEWT_VAL(BLE_GATTC)
    int rc = ble_gattc_exchange_mtu(s_conn, NULL, NULL);
    if (rc != 0 && rc != BLE_HS_EALREADY) {
        ESP_LOGD(TAG, "mtu exchange rc=%d", rc);
    }
#else
    /* 每个连接最多两次（连接事件 + 上传开始），只打 debug 不刷屏。 */
    ESP_LOGD(TAG, "mtu=%u, gatt client disabled: wait for central exchange",
             (unsigned)ble_att_mtu(s_conn));
#endif
}

/* ------------------------------------------------------------------ *
 * 通知
 * ------------------------------------------------------------------ */
/* 订阅类通知的节流判据：值变化，或距上次成功发送 >= 60 s。 */
static bool notify_due(int16_t value, int16_t last, uint32_t last_ms)
{
    uint32_t now = (uint32_t)rc_now_ms();
    if (last == value && last_ms != 0 &&
        (now - last_ms) < MP_BLE_NOTIFY_PERIOD_MS) {
        return false;
    }
    return true;
}

/* 一条 notify。长度必须 <= MTU-3：超长会被 NimBLE 静默截断
 * （ble_att_truncate_to_mtu），宁可直接失败也不要发半条（0x33 回读靠这个）。
 * 任意任务可调：NimBLE 的发送路径内部拿 host 锁（ble_att_cmd.c:115）。 */
static int notify_handle(uint16_t handle, const void *data, uint16_t len)
{
    if (handle == 0 || data == NULL || len == 0 ||
        len > (uint16_t)MP_BLE_PKT_MAX) {
        return -1;
    }
    if (!s_connected || s_conn == BLE_HS_CONN_HANDLE_NONE) {
        return -1;
    }

    uint16_t mtu = ble_att_mtu(s_conn);
    if (mtu < MP_BLE_MTU_MIN) {
        mtu = MP_BLE_MTU_MIN;
    }
    if (len > (uint16_t)(mtu - 3)) {
        return -1;
    }

    struct os_mbuf *om = ble_hs_mbuf_from_flat(data, len);
    if (om == NULL) {
        log_limited(&s_notify_errs, "notify: out of mbufs", 0);
        return -3;
    }
    /* 成功/失败都消费 om（ble_gatt.h:900）。 */
    int rc = ble_gatts_notify_custom(s_conn, handle, om);
    if (rc != 0) {
        log_limited(&s_notify_errs, "notify", rc);
        return -4;
    }
    return 0;
}

int mp_ble_notify(const void *data, uint16_t len)
{
    return notify_handle(s_h_main, data, len);
}

/* ------------------------------------------------------------------ *
 * GATT 访问回调
 * ------------------------------------------------------------------ */
/* 0x331F：书 / 字库 / 锁图 / 设置 / 调试（ebook_ble.c 的 switch）。
 * 这里只做"拷进队列"；例外是 0x42-0x44 就地回 GPIO5 电平（PORT_SPEC D13），
 * 以及队列满时限频打一条日志。 */
static int chr_main_access(uint16_t conn_handle, uint16_t attr_handle,
                           struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)conn_handle;
    (void)attr_handle;
    (void)arg;

    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) {
        return BLE_ATT_ERR_UNLIKELY;
    }

    uint8_t pkt[MP_BLE_PKT_MAX];
    uint16_t len = 0;
    if (ble_hs_mbuf_to_flat(ctxt->om, pkt, sizeof pkt, &len) != 0 || len == 0) {
        return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    }

    uint8_t op = pkt[0];
    /* 路由：放行 0x10..0x44 原版 Miaopaper 协议以及 0x50..0x54 固件原地自更新协议。 */
    bool op_ok = (op >= MP_BLE_OP_MIN && op <= MP_BLE_OP_MAX) ||
                 (op >= MP_OP_OTA_INFO && op <= MP_OP_OTA_ABORT);
    if (!op_ok) {
        return 0;
    }

    /* 0x42/0x43/0x44：E2S 没有 PC1，回充电检测脚 GPIO5 的电平
     * （1=高、0=低；PORT_SPEC §7.3 D13）。**不驱动、不改引脚配置**。 */
    if (op >= MP_OP_PC1_WEAK_PD && op <= MP_OP_PC1_HIZ) {
        uint8_t rsp[2] = {
            op,
            (uint8_t)(gpio_get_level(BOARD_CHARGE_DET_GPIO) ? 1 : 0),
        };
        (void)notify_handle(s_h_main, rsp, (uint16_t)sizeof rsp);
        return 0;
    }

    mp_ble_req_t req;
    req.op = op;
    req.len = (uint8_t)(len - 1);          /* op 之后的字节数（mp_ble.h） */
    if (req.len > 0) {
        memcpy(req.data, pkt + 1, req.len);
    }

    if (s_req_q == NULL || xQueueSend(s_req_q, &req, 0) != pdPASS) {
        unsigned n = atomic_fetch_add(&s_drop_count, 1) + 1;
        if (n == 1 || (n % 256u) == 0) {
            ESP_LOGW(TAG, "req queue full, dropped=%u", n);
        }
        /* WRITE_REQ 时主机能看到错误；WRITE_NO_RSP 没有回包路径。 */
        return BLE_ATT_ERR_INSUFFICIENT_RES;
    }
    atomic_fetch_add(&s_rx_count, 1);
    notify_app_wake();
    return 0;
}

/* 0x1F1F：只认对时 0xDD。参考固件在这里还挂了 0xE2/0xED/0xF3… 调试命令
 * 和日志回显，本工程一律忽略（PORT_SPEC §7.5 最后一条）。 */
static int chr_rxtx_access(uint16_t conn_handle, uint16_t attr_handle,
                           struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)conn_handle;
    (void)attr_handle;
    (void)arg;

    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) {
        return BLE_ATT_ERR_UNLIKELY;
    }

    uint8_t pkt[32];
    uint16_t len = 0;
    if (ble_hs_mbuf_to_flat(ctxt->om, pkt, sizeof pkt, &len) != 0 || len == 0) {
        return 0;                          /* 兼容性：不因调试包回 ATT 错误 */
    }
    if (pkt[0] != MP_OP_SET_TIME || len != 10) {
        return 0;                          /* cmd_parser.c:73-75 的固定布局 */
    }

    mp_time_t t;
    t.unix = ((uint32_t)pkt[1] << 24) | ((uint32_t)pkt[2] << 16) |
             ((uint32_t)pkt[3] << 8) | (uint32_t)pkt[4];
    t.year = (uint16_t)(((uint16_t)pkt[5] << 8) | pkt[6]);
    t.month = pkt[7];
    t.day = pkt[8];
    t.week = pkt[9];

    /* web 永远发合法值；越界的 month/day 会让 app 的进位逻辑出错，直接丢弃
     * （时钟继续走默认值，比把日期写成 0 月安全）。week 越界夹到 1。 */
    if (t.month < 1 || t.month > 12 || t.day < 1 || t.day > 31) {
        ESP_LOGD(TAG, "0xDD rejected: month=%u day=%u",
                 (unsigned)t.month, (unsigned)t.day);
        return 0;
    }
    if (t.week < 1 || t.week > 7) {
        t.week = 1;
    }
    t.sec = (uint8_t)(t.unix % 60u);
    t.min = (uint8_t)((t.unix / 60u) % 60u);
    t.hour = (uint8_t)((t.unix / 3600u) % 24u);

    /* 回调只允许置标志；真正的 mp_app 更新在 app 任务（PORT_SPEC §7.5）。 */
    void (*cb)(const mp_time_t *) = s_time_cb;
    if (cb != NULL) {
        cb(&t);
        notify_app_wake();
    }
    return 0;
}

static int chr_batt_access(uint16_t conn_handle, uint16_t attr_handle,
                           struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)conn_handle;
    (void)attr_handle;
    (void)arg;
    if (ctxt->op != BLE_GATT_ACCESS_OP_READ_CHR) {
        return BLE_ATT_ERR_UNLIKELY;
    }
    uint8_t v = s_batt_pct;
    return os_mbuf_append(ctxt->om, &v, 1) == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
}

static int chr_temp_access(uint16_t conn_handle, uint16_t attr_handle,
                           struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)conn_handle;
    (void)attr_handle;
    (void)arg;
    if (ctxt->op != BLE_GATT_ACCESS_OP_READ_CHR) {
        return BLE_ATT_ERR_UNLIKELY;
    }
    /* 0x2A1F 是 int16 LE（app_att.c:72 的 my_tempVal 是 {lo, hi}）。 */
    uint16_t u = (uint16_t)s_temp_c;
    uint8_t v[2] = {(uint8_t)(u & 0xFF), (uint8_t)(u >> 8)};
    return os_mbuf_append(ctxt->om, v, sizeof v) == 0
               ? 0
               : BLE_ATT_ERR_INSUFFICIENT_RES;
}

static int chr_pnp_access(uint16_t conn_handle, uint16_t attr_handle,
                          struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)conn_handle;
    (void)attr_handle;
    (void)arg;
    if (ctxt->op != BLE_GATT_ACCESS_OP_READ_CHR) {
        return BLE_ATT_ERR_UNLIKELY;
    }
    return os_mbuf_append(ctxt->om, s_pnp_id, sizeof s_pnp_id) == 0
               ? 0
               : BLE_ATT_ERR_INSUFFICIENT_RES;
}

/* ------------------------------------------------------------------ *
 * GATT 表（app_att.c:63-94 / 160-204 的 16-bit 服务；128-bit EPD 推屏
 * 服务不移植——本工程不用它）
 * ------------------------------------------------------------------ */
static const ble_uuid16_t s_uuid_svc_main = BLE_UUID16_INIT(MP_BLE_SVC_MAIN);
static const ble_uuid16_t s_uuid_chr_main = BLE_UUID16_INIT(MP_BLE_CHR_MAIN);
static const ble_uuid16_t s_uuid_svc_rxtx = BLE_UUID16_INIT(MP_BLE_SVC_RXTX);
static const ble_uuid16_t s_uuid_chr_rxtx = BLE_UUID16_INIT(MP_BLE_CHR_RXTX);
static const ble_uuid16_t s_uuid_svc_batt = BLE_UUID16_INIT(MP_BLE_SVC_BATT);
static const ble_uuid16_t s_uuid_chr_batt = BLE_UUID16_INIT(MP_BLE_CHR_BATT);
static const ble_uuid16_t s_uuid_svc_env = BLE_UUID16_INIT(MP_BLE_SVC_ENV);
static const ble_uuid16_t s_uuid_chr_temp = BLE_UUID16_INIT(MP_BLE_CHR_TEMP);
static const ble_uuid16_t s_uuid_svc_devi = BLE_UUID16_INIT(MP_BLE_SVC_DEVI);
static const ble_uuid16_t s_uuid_chr_pnp = BLE_UUID16_INIT(MP_BLE_CHR_PNP);

static const struct ble_gatt_chr_def s_chrs_main[] = {
    {
        .uuid = &s_uuid_chr_main.u,
        .access_cb = chr_main_access,
        .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP |
                 BLE_GATT_CHR_F_NOTIFY,
        .val_handle = &s_h_main,
    },
    {0},
};

static const struct ble_gatt_chr_def s_chrs_rxtx[] = {
    {
        .uuid = &s_uuid_chr_rxtx.u,
        .access_cb = chr_rxtx_access,
        /* NOTIFY 位必须保留（本工程不发通知）：原版 web 的 setupRxTx()
         * 无条件 startNotifications()，缺 notify 属性会被浏览器拒绝，
         * 整条连接链进 catch 并断连（web:322-325、web:313）。 */
        .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP |
                 BLE_GATT_CHR_F_NOTIFY,
        .val_handle = &s_h_rxtx,
    },
    {0},
};

static const struct ble_gatt_chr_def s_chrs_batt[] = {
    {
        .uuid = &s_uuid_chr_batt.u,
        .access_cb = chr_batt_access,
        .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY,
        .val_handle = &s_h_batt,
    },
    {0},
};

static const struct ble_gatt_chr_def s_chrs_env[] = {
    {
        .uuid = &s_uuid_chr_temp.u,
        .access_cb = chr_temp_access,
        .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY,
        .val_handle = &s_h_temp,
    },
    {0},
};

static const struct ble_gatt_chr_def s_chrs_devi[] = {
    {
        .uuid = &s_uuid_chr_pnp.u,
        .access_cb = chr_pnp_access,
        .flags = BLE_GATT_CHR_F_READ,
    },
    {0},
};

static const struct ble_gatt_svc_def s_svcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &s_uuid_svc_main.u,
        .characteristics = s_chrs_main,
    },
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &s_uuid_svc_rxtx.u,
        .characteristics = s_chrs_rxtx,
    },
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &s_uuid_svc_batt.u,
        .characteristics = s_chrs_batt,
    },
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &s_uuid_svc_env.u,
        .characteristics = s_chrs_env,
    },
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &s_uuid_svc_devi.u,
        .characteristics = s_chrs_devi,
    },
    {0},
};

static int gatt_register(void)
{
    ble_svc_gap_init();
    ble_svc_gatt_init();

    int rc = ble_gatts_count_cfg(s_svcs);
    if (rc != 0) {
        ESP_LOGE(TAG, "gatts_count_cfg rc=%d", rc);
        return -1;
    }
    rc = ble_gatts_add_svcs(s_svcs);
    if (rc != 0) {
        ESP_LOGE(TAG, "gatts_add_svcs rc=%d", rc);
        return -1;
    }
    return 0;
}

/* ------------------------------------------------------------------ *
 * GAP 事件
 * ------------------------------------------------------------------ */
static int gap_event(struct ble_gap_event *event, void *arg)
{
    (void)arg;

    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0) {
            s_conn = event->connect.conn_handle;
            s_connected = true;
            /* 连接建立时控制器已停止可连接广播；不改 s_adv_wanted。 */
            s_adv_on = false;
            s_param_req_ms = 0;
            ESP_LOGI(TAG, "connected");
            request_fast_params();
            exchange_mtu_best_effort();
            if (s_conn_cb != NULL) {
                s_conn_cb(1);
            }
            notify_app_wake();
        } else {
            ESP_LOGW(TAG, "connect failed status=%d", event->connect.status);
            adv_apply();
        }
        return 0;

    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGI(TAG, "disconnected reason=0x%x", event->disconnect.reason);
        s_connected = false;
        s_conn = BLE_HS_CONN_HANDLE_NONE;
        s_batt_sub = 0;
        s_temp_sub = 0;
        s_batt_notified = -1;
        s_batt_notify_ms = 0;
        s_temp_notified = 0x7FFF;
        s_temp_notify_ms = 0;

        /* 断连 = 上传会话中止（PORT_SPEC §7.4）：丢掉还没处理的包，
         * 否则重连后 app 会把上一会话的数据当成新会话的 0x11。 */
        if (s_req_q != NULL) {
            xQueueReset(s_req_q);
        }
        if (s_conn_cb != NULL) {
            s_conn_cb(0);
        }
        notify_app_wake();
        adv_apply(); /* s_adv_wanted=0 时不重开（"BLE 关闭时停广播"） */
        return 0;

    case BLE_GAP_EVENT_ADV_COMPLETE:
        s_adv_on = false;
        adv_apply();
        return 0;

    case BLE_GAP_EVENT_CONN_UPDATE:
        if (event->conn_update.status == 0) {
            ESP_LOGD(TAG, "conn params updated");
        } else {
            ESP_LOGD(TAG, "conn update failed status=%d",
                     event->conn_update.status);
        }
        return 0;

    case BLE_GAP_EVENT_MTU:
        /* 只报数值（不含地址/身份）。0x33 回读要求 MTU>=166（§7.7）。 */
        ESP_LOGI(TAG, "att mtu=%u", (unsigned)event->mtu.value);
        return 0;

    case BLE_GAP_EVENT_SUBSCRIBE:
        if (event->subscribe.attr_handle == s_h_batt) {
            s_batt_sub = event->subscribe.cur_notify ? 1 : 0;
        } else if (event->subscribe.attr_handle == s_h_temp) {
            s_temp_sub = event->subscribe.cur_notify ? 1 : 0;
        }
        return 0;

    default:
        return 0;
    }
}

/* ------------------------------------------------------------------ *
 * NimBLE host 生命周期
 * ------------------------------------------------------------------ */
static void on_host_reset(int reason)
{
    ESP_LOGW(TAG, "host reset reason=%d", reason);
    s_synced = false;
    s_connected = false;
    s_adv_on = false;
    s_conn = BLE_HS_CONN_HANDLE_NONE;
    s_batt_sub = 0;
    s_temp_sub = 0;
    s_batt_notified = -1;
    s_batt_notify_ms = 0;
    s_temp_notified = 0x7FFF;
    s_temp_notify_ms = 0;
    if (s_conn_cb != NULL) {
        s_conn_cb(0); /* 上传会话要收尾（§7.4） */
    }
}

static void on_host_sync(void)
{
    /* 只推导地址类型，**不读也不打印** MAC（P §11#5/§12#43）。 */
    int rc = ble_hs_id_infer_auto(0, &s_addr_type);
    if (rc != 0) {
        s_addr_type = BLE_OWN_ADDR_PUBLIC;
    }
    s_synced = true;
    adv_apply();
}

static void host_task(void *param)
{
    (void)param;
    nimble_port_run(); /* 只在 nimble_port_stop() 后返回 */
    if (s_host_exit_sem != NULL) {
        xSemaphoreGive(s_host_exit_sem);
    }
    nimble_port_freertos_deinit();
}

/* ------------------------------------------------------------------ *
 * 公共 API
 * ------------------------------------------------------------------ */
int mp_ble_start(void)
{
    if (s_started) {
        return 0;
    }

    /* PORT_SPEC §10.1 例外 2：整个工程只有这一处读 mp_settings。
     * main.c 已保证 mp_settings_defaults() 先于本函数执行（§6.7）。 */
    s_adv_wanted = mp_settings.ble_enabled ? true : false;
    s_synced = false;
    s_connected = false;
    s_adv_on = false;
    s_conn = BLE_HS_CONN_HANDLE_NONE;
    s_upload = 0;
    adv_payload_build();

    if (s_req_q == NULL) {
        s_req_q = xQueueCreate(MP_BLE_REQ_QUEUE, sizeof(mp_ble_req_t));
    }
    if (s_adv_mtx == NULL) {
        s_adv_mtx = xSemaphoreCreateMutex();
    }
    if (s_host_exit_sem == NULL) {
        s_host_exit_sem = xSemaphoreCreateBinary();
    }
    if (s_req_q == NULL || s_adv_mtx == NULL || s_host_exit_sem == NULL) {
        ESP_LOGE(TAG, "no memory for BLE queue/mutex");
        return -1;
    }
    (void)xSemaphoreTake(s_host_exit_sem, 0);

    esp_err_t e = nimble_port_init();
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "nimble_port_init: %s", esp_err_to_name(e));
        return -2;
    }

    ble_hs_cfg.reset_cb = on_host_reset;
    ble_hs_cfg.sync_cb = on_host_sync;
    /* No_Security：不配对、不绑定、不加密（ble.c:263）。NVS 持久化在
     * sdkconfig 里关掉，store 只用 RAM，本文件不写内部 flash/NVS。 */
    ble_hs_cfg.sm_io_cap = BLE_SM_IO_CAP_NO_IO;
    ble_hs_cfg.sm_bonding = 0;
    ble_hs_cfg.sm_mitm = 0;
    ble_hs_cfg.sm_sc = 0;

    if (gatt_register() != 0) {
        (void)nimble_port_deinit();
        return -3;
    }

    /* Device Name 特征值 "ESL"（app_att.c:58）；广播名另见 adv_set_fields()。 */
    int nrc = ble_svc_gap_device_name_set(MP_BLE_GAP_NAME);
    if (nrc != 0) {
        ESP_LOGW(TAG, "gap name rc=%d", nrc);
    }

    /* 247 与 CONFIG_BT_NIMBLE_ATT_PREFERRED_MTU 一致；显式设一次以防
     * sdkconfig 被改小（web 的 0x33 预览需要 MTU>=166）。 */
    (void)ble_att_set_preferred_mtu(MP_BLE_MTU_REQ);

    /* CONFIG_BT_NIMBLE_NVS_PERSIST=n → 只用 RAM 数组（reader_ble.c:48）。 */
    ble_store_config_init();

    s_started = true;
    nimble_port_freertos_init(host_task);

    ESP_LOGI(TAG, "ready: %s, adv %s", MP_BLE_NAME, s_adv_wanted ? "on" : "off");
    return 0;
}

void mp_ble_stop(void)
{
    if (!s_started) {
        return;
    }

    s_adv_wanted = false;
    adv_apply();

    if (s_connected && s_conn != BLE_HS_CONN_HANDLE_NONE) {
        (void)ble_gap_terminate(s_conn, BLE_ERR_REM_USER_CONN_TERM);
    }

    int rc = nimble_port_stop();
    if (rc != 0) {
        ESP_LOGW(TAG, "nimble_port_stop rc=%d", rc);
    } else if (s_host_exit_sem != NULL) {
        (void)xSemaphoreTake(s_host_exit_sem, pdMS_TO_TICKS(1000));
    }
    /* 等待高优先级 nimble_host 完成自删，防止 deinit 期间上下文仍被访问。 */
    vTaskDelay(pdMS_TO_TICKS(20));

    esp_err_t de = nimble_port_deinit();
    if (de != ESP_OK) {
        ESP_LOGW(TAG, "nimble_port_deinit: %s", esp_err_to_name(de));
    }

    s_started = false;
    s_synced = false;
    s_connected = false;
    s_adv_on = false;
    s_conn = BLE_HS_CONN_HANDLE_NONE;
    s_upload = 0;
    s_batt_sub = 0;
    s_temp_sub = 0;
    s_batt_notified = -1;
    s_batt_notify_ms = 0;
    s_temp_notified = 0x7FFF;
    s_temp_notify_ms = 0;
    s_h_main = 0;
    s_h_rxtx = 0;
    s_h_batt = 0;
    s_h_temp = 0;
    if (s_req_q != NULL) {
        xQueueReset(s_req_q);
    }
    ESP_LOGI(TAG, "ble stack stopped (radio off)");
}

uint8_t mp_ble_running(void)
{
    return s_started ? 1 : 0;
}

void mp_ble_adv(uint8_t on)
{
    bool want = on ? true : false;
    if (s_adv_wanted != want) {
        s_adv_wanted = want;
    }
    if (s_started) {
        adv_apply();
    }
}

uint8_t mp_ble_connected(void)
{
    return s_connected ? 1 : 0;
}

uint16_t mp_ble_mtu(void)
{
    if (s_connected && s_conn != BLE_HS_CONN_HANDLE_NONE) {
        uint16_t mtu = ble_att_mtu(s_conn);
        if (mtu >= MP_BLE_MTU_MIN) {
            return mtu;
        }
    }
    return MP_BLE_MTU_MIN;
}

uint8_t mp_ble_req_get(mp_ble_req_t *out, uint32_t timeout_ms)
{
    if (out == NULL || s_req_q == NULL) {
        return 0;
    }
    TickType_t ticks = 0;
    if (timeout_ms > 0) {
        ticks = pdMS_TO_TICKS(timeout_ms);
        if (ticks == 0) {
            ticks = 1; /* 亚 tick 超时也要让出一次 */
        }
    }
    return xQueueReceive(s_req_q, out, ticks) == pdPASS ? 1 : 0;
}

void mp_ble_set_upload_active(uint8_t on)
{
    uint8_t v = on ? 1 : 0;
    if (s_upload == v) {
        return;
    }
    s_upload = v;
    if (!s_started) {
        return;
    }
    if (v) {
        /* 上传前把链路压到 7.5–10 ms 并确认 MTU（ble.c:187-190、
         * ebook_ble.c:234/650）。这是"上传通知"的链路侧部分：
         * 数据水位通知由 app 调 mp_ble_notify_written()。 */
        request_fast_params();
        exchange_mtu_best_effort();
    } else {
        /* 上传结束立即切回省电连接参数，降低射频保活功耗。 */
        request_idle_params();
    }
}

uint8_t mp_ble_upload_active(void)
{
    return s_upload;
}

void mp_ble_stats(mp_ble_stats_t *out)
{
    if (out == NULL) {
        return;
    }
    memset(out, 0, sizeof *out);
    out->rx = (uint32_t)atomic_load(&s_rx_count);
    out->dropped = (uint32_t)atomic_load(&s_drop_count);
    out->mtu = mp_ble_mtu();
    out->connected = s_connected ? 1 : 0;
    out->adv = s_adv_on ? 1 : 0;
}

void mp_ble_set_time_cb(void (*cb)(const mp_time_t *t))
{
    s_time_cb = cb;
}

void mp_ble_set_conn_cb(void (*cb)(uint8_t connected))
{
    s_conn_cb = cb;
}

void mp_ble_set_wake_cb(void (*cb)(void))
{
    s_wake_cb = cb;
}

void mp_ble_publish_battery(uint8_t percent, uint16_t mv)
{
    if (percent > 100) {
        percent = 100;
    }
    bool changed = (percent != s_batt_pct) || (mv != s_batt_mv);
    s_batt_pct = percent;
    s_batt_mv = mv;

    if (!s_started) {
        return;
    }
    adv_maybe_refresh(changed);

    if (s_batt_sub &&
        notify_due((int16_t)percent, s_batt_notified, s_batt_notify_ms)) {
        uint8_t v = percent;
        if (notify_handle(s_h_batt, &v, 1) == 0) {
            s_batt_notified = (int16_t)percent;
            s_batt_notify_ms = (uint32_t)rc_now_ms();
        }
    }
}

void mp_ble_publish_temp(int16_t celsius)
{
    bool changed = (celsius != s_temp_c);
    s_temp_c = celsius;

    if (!s_started) {
        return;
    }
    adv_maybe_refresh(changed);

    if (s_temp_sub &&
        notify_due(celsius, s_temp_notified, s_temp_notify_ms)) {
        uint16_t u = (uint16_t)celsius;
        uint8_t v[2] = {(uint8_t)(u & 0xFF), (uint8_t)(u >> 8)};
        if (notify_handle(s_h_temp, v, (uint16_t)sizeof v) == 0) {
            s_temp_notified = celsius;
            s_temp_notify_ms = (uint32_t)rc_now_ms();
        }
    }
}
