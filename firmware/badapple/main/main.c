/* Bad Apple on E2S —— 先启动 BLE 恢复服务，再进入动画回放。 */
#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"

#include "a5_server.h"
#include "board.h"
#include "demo_config.h"
#include "key.h"
#include "playback.h"

#include "nimble/nimble_port.h"
#include "host/ble_hs.h"
#include "host/ble_gap.h"
#include "host/util/util.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

static const char *TAG = "main";
static const char DEV_ADV_NAME[] = DEMO_BLE_NAME;

/* 0000FFxx-C243-40B9-AC41-A10BA3AD5D1A；整个 UUID 在 NimBLE 中按小端存储。 */
#define E2S_UUID(lo) BLE_UUID128_INIT(0x1a, 0x5d, 0xad, 0xa3, 0x0b, 0xa1, 0x41, 0xac, \
                                      0xb9, 0x40, 0x43, 0xc2, lo,   0xff, 0x00, 0x00)
static const ble_uuid128_t uuid_svc     = E2S_UUID(0x00);
static const ble_uuid128_t uuid_ntf_mcu = E2S_UUID(0x01);
static const ble_uuid128_t uuid_write   = E2S_UUID(0x02);
static const ble_uuid128_t uuid_ntf_ble = E2S_UUID(0x03);

static uint16_t chr_val_handles[3]; /* [0]=FF01 [1]=FF02 [2]=FF03 */
static uint16_t s_conn = BLE_HS_CONN_HANDLE_NONE;
static bool s_ff01, s_ff03, s_link_failed;
static struct ble_npl_event s_adv_event;
static StaticEventGroup_t s_ble_ready_storage;
static EventGroupHandle_t s_ble_ready;
static volatile bool s_need_adv = true;   /* 未连接时必须有广播在跑 */
static volatile int s_adv_rc = -999;      /* 最近一次 ble_gap_adv_start 返回码 */
static volatile unsigned s_n_conn, s_n_disc, s_n_advc, s_n_restart;
static esp_timer_handle_t s_adv_retry;
static uint8_t s_own_addr_type = BLE_OWN_ADDR_PUBLIC;
/* 待机时开广播，播放时关广播；由回放任务切换。 */
static volatile bool s_adv_enabled = true;
/* 启动阶段码，画在开机条纹上：1=广播已起 2=nimble init 失败 3=GATT 配置失败
 * 4=host 任务创建失败 5=广播 start 失败 6=host 2 秒内未同步。 */
static volatile int s_boot_status = 6;
int ble_boot_status(void) { return s_boot_status; }

static uint16_t segment_size(void)
{
    uint16_t mtu = ble_att_mtu(s_conn);
    return mtu >= BLE_ATT_MTU_DFLT ? mtu - 3 : BLE_ATT_MTU_DFLT - 3;
}

static int notify_chr(unsigned index, const uint8_t *data, uint16_t len)
{
    if (s_conn == BLE_HS_CONN_HANDLE_NONE || !chr_val_handles[index]) return BLE_HS_ENOTCONN;
    struct os_mbuf *om = ble_hs_mbuf_from_flat(data, len);
    return om ? ble_gatts_notify_custom(s_conn, chr_val_handles[index], om) : BLE_HS_ENOMEM;
}

static void notify_failed(int rc)
{
    ESP_LOGW(TAG, "notify failed: %d", rc);
    /* 不能把半条应答留给同一会话的下一条命令。等当前解析返回后再重置。 */
    s_link_failed = true;
    s_ff01 = s_ff03 = false;
    rc = ble_gap_terminate(s_conn, BLE_ERR_REM_USER_CONN_TERM);
    if (rc && rc != BLE_HS_ENOTCONN) ESP_LOGW(TAG, "terminate: %d", rc);
}

static void send_ready_notices(void)
{
    if (!s_ff01 || !s_ff03 || s_link_failed) return;
    static const uint8_t type_msg[] = { 1, 3 };
    uint16_t seg = segment_size();
    uint8_t mtu_msg[] = { 2, (uint8_t)seg, (uint8_t)(seg >> 8) };
    int rc = notify_chr(2, type_msg, sizeof(type_msg));
    if (!rc) rc = notify_chr(2, mtu_msg, sizeof(mtu_msg));
    if (rc) notify_failed(rc);
}

static void a5_notify(const uint8_t *data, uint16_t len)
{
    if (!s_ff01 || s_link_failed || s_conn == BLE_HS_CONN_HANDLE_NONE) return;
    uint16_t lim = segment_size();
    while (len) {
        uint16_t n = len < lim ? len : lim;
        int rc = notify_chr(0, data, n);
        if (rc) { notify_failed(rc); return; }
        data += n;
        len -= n;
    }
}

static int chr_access_cb(uint16_t conn_handle, uint16_t attr_handle,
                         struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)arg;
    if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR && attr_handle == chr_val_handles[1]) {
        if (s_link_failed || conn_handle != s_conn || !s_ff01 || !s_ff03) return BLE_ATT_ERR_UNLIKELY;
        uint16_t len = OS_MBUF_PKTLEN(ctxt->om);
        uint8_t buf[CONFIG_BT_NIMBLE_ATT_PREFERRED_MTU - 3];
        if (!len || len > sizeof(buf) || len > segment_size()) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
        if (os_mbuf_copydata(ctxt->om, 0, len, buf)) return BLE_ATT_ERR_UNLIKELY;
        if (!a5_server_on_ble_write(buf, len)) {
            ESP_LOGW(TAG, "malformed A5 request");
            return BLE_ATT_ERR_UNLIKELY;
        }
        if (s_link_failed) a5_server_disconnect();
        return 0;
    }
    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR)
        return os_mbuf_append(ctxt->om, "\x00", 1) ? BLE_ATT_ERR_INSUFFICIENT_RES : 0;
    return BLE_ATT_ERR_UNLIKELY;
}

static const struct ble_gatt_svc_def gatt_svcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &uuid_svc.u,
        .characteristics = (struct ble_gatt_chr_def[]){
            { .uuid = &uuid_ntf_mcu.u, .access_cb = chr_access_cb,
              .val_handle = &chr_val_handles[0],
              .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY },
            { .uuid = &uuid_write.u, .access_cb = chr_access_cb,
              .val_handle = &chr_val_handles[1],
              .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP },
            { .uuid = &uuid_ntf_ble.u, .access_cb = chr_access_cb,
              .val_handle = &chr_val_handles[2],
              .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY },
            { 0 }
        },
    },
    { 0 }
};

static void clear_connection(void)
{
    s_conn = BLE_HS_CONN_HANDLE_NONE;
    s_ff01 = s_ff03 = s_link_failed = false;
    a5_server_disconnect();
}

static void queue_advertising(void)
{
    ble_npl_eventq_put(nimble_port_get_dflt_eventq(), &s_adv_event);
}

/* 广播起不来时的重试：定时器只投递事件，真正的 GAP 调用留在 host 任务。 */
static void adv_retry_cb(void *arg)
{
    (void)arg;
    if (ble_hs_synced() && s_conn == BLE_HS_CONN_HANDLE_NONE) queue_advertising();
}

void ble_set_advertising(bool on)
{
    s_adv_enabled = on;
    s_need_adv = on;
    queue_advertising();   /* host 任务里按 s_adv_enabled 启停 */
}

/* 回放任务定期调用：这里只读状态并投递事件，绝不在别的任务里直接调用 GAP。
 * NimBLE 的广播 start/stop 全部留在 host 任务执行（ble_ads_start / gap_event_cb）。
 * 部分手机在 GATT 会话被强杀时可能只断开 ACL，NimBLE 收不到 DISCONNECT。 */
void ble_service_tick(void)
{
    if (!s_adv_enabled) return;
    if (!ble_hs_synced()) return;
    if (s_conn != BLE_HS_CONN_HANDLE_NONE) {
        struct ble_gap_conn_desc desc;
        if (ble_gap_conn_find(s_conn, &desc) != 0) {
            ESP_LOGW(TAG, "stale connection %d, re-advertising", s_conn);
            s_n_restart++;
            clear_connection();
            s_need_adv = true;
            queue_advertising();
        }
        return;
    }
    if (!ble_gap_adv_active()) {
        s_need_adv = true;
        queue_advertising();
    }
}

void ble_diag(char *out, int n)
{
    char fr[64];
    playback_diag(fr, sizeof(fr));
    snprintf(out, n, "ble: synced=%d conn=%d adv=%d need=%d en=%d rc=%d c=%u d=%u ac=%u rst=%u key=%x %s",
             ble_hs_synced(), (int)s_conn, ble_gap_adv_active(), s_need_adv, s_adv_enabled,
             s_adv_rc, s_n_conn, s_n_disc, s_n_advc, s_n_restart, (unsigned)key_mask(), fr);
}

static int gap_event_cb(struct ble_gap_event *event, void *arg)
{
    (void)arg;
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        s_n_conn++;
        if (!event->connect.status) {
            clear_connection();
            s_conn = event->connect.conn_handle;
            s_need_adv = false;
        } else {
            ESP_LOGW(TAG, "BLE connect failed: %d", event->connect.status);
            s_need_adv = true;
            queue_advertising();
        }
        break;
    case BLE_GAP_EVENT_DISCONNECT:
        s_n_disc++;
        if (event->disconnect.conn.conn_handle == s_conn) {
            clear_connection();
            s_need_adv = true;
            /* 广播的启停只在 host 任务里做，这里只投递事件。 */
            queue_advertising();
        }
        break;
    case BLE_GAP_EVENT_ADV_COMPLETE:
        s_n_advc++;
        if (s_conn == BLE_HS_CONN_HANDLE_NONE) {
            s_need_adv = true;
            queue_advertising();
        }
        break;
    case BLE_GAP_EVENT_SUBSCRIBE:
        if (event->subscribe.conn_handle != s_conn) break;
        if (event->subscribe.attr_handle == chr_val_handles[0])
            s_ff01 = event->subscribe.cur_notify;
        else if (event->subscribe.attr_handle == chr_val_handles[2])
            s_ff03 = event->subscribe.cur_notify;
        else break;
        if (s_ff01 && s_ff03) send_ready_notices();
        else a5_server_disconnect();
        break;
    case BLE_GAP_EVENT_MTU:
        if (event->mtu.conn_handle == s_conn && event->mtu.channel_id == BLE_L2CAP_CID_ATT)
            send_ready_notices();
        break;
    default:
        break;
    }
    return 0;
}

static void ble_ads_start(struct ble_npl_event *event)
{
    (void)event;
    if (!ble_hs_synced() || s_conn != BLE_HS_CONN_HANDLE_NONE) return;
    if (!s_adv_enabled) {
        if (ble_gap_adv_active()) ble_gap_adv_stop();
        s_need_adv = false;
        return;
    }
    if (ble_gap_adv_active()) {
        if (!s_need_adv) {              /* 已经在广播，什么都不用做 */
            xEventGroupSetBits(s_ble_ready, BIT0);
            return;
        }
        /* 状态陈旧（例如 DISCONNECT 丢了）：在 host 任务里停掉再重起。 */
        ble_gap_adv_stop();
    }
    if (!chr_val_handles[0] || !chr_val_handles[1] || !chr_val_handles[2]) {
        ESP_LOGE(TAG, "A5 GATT handles not registered");
        s_need_adv = true;
        return;
    }
    struct ble_gap_adv_params adv = { 0 };
    struct ble_hs_adv_fields fields = { 0 };
    struct ble_hs_adv_fields scan_rsp = { 0 };
    /* 传统广播最大 31B：标志+全名 17B，UUID 放进单独的扫描响应（18B）。 */
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.name = (const uint8_t *)DEV_ADV_NAME;
    fields.name_len = sizeof(DEV_ADV_NAME) - 1;
    fields.name_is_complete = 1;
    scan_rsp.uuids128 = &uuid_svc;
    scan_rsp.num_uuids128 = 1;
    scan_rsp.uuids128_is_complete = 1;
    adv.itvl_min = 40; /* 25 ms */
    adv.itvl_max = 80;
    adv.conn_mode = BLE_GAP_CONN_MODE_UND;
    adv.disc_mode = BLE_GAP_DISC_MODE_GEN;
    int rc = ble_gap_adv_set_fields(&fields);
    if (!rc) rc = ble_gap_adv_rsp_set_fields(&scan_rsp);
    if (!rc) rc = ble_gap_adv_start(s_own_addr_type, NULL, BLE_HS_FOREVER, &adv, gap_event_cb, NULL);
    s_adv_rc = rc;
    if (rc) {
        ESP_LOGE(TAG, "start advertising: %d", rc);
        s_need_adv = true;
        if (s_adv_retry) esp_timer_start_once(s_adv_retry, 500000);
    } else {
        s_need_adv = false;
        xEventGroupSetBits(s_ble_ready, BIT0);
    }
}

static void on_sync(void)
{
    /* 和 ESP-IDF 自带 bleprph 一致：先确保控制器有公共地址，再推断本机地址类型。
     * 少了这一步时 ble_gap_adv_start(BLE_OWN_ADDR_PUBLIC) 可能一直失败。 */
    int rc = ble_hs_util_ensure_addr(0);
    if (rc) ESP_LOGE(TAG, "ensure addr: %d", rc);
    rc = ble_hs_id_infer_auto(0, &s_own_addr_type);
    if (rc) {
        ESP_LOGE(TAG, "infer addr type: %d", rc);
        s_own_addr_type = BLE_OWN_ADDR_PUBLIC;
    }
    queue_advertising();
}

static void on_reset(int reason)
{
    ESP_LOGW(TAG, "host reset %d", reason);
    clear_connection();
}

static void ble_host_task(void *param)
{
    (void)param;
    nimble_port_run();
    vTaskDelete(NULL);
}

static bool ble_init(void)
{
    esp_err_t err = nimble_port_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nimble init: %s", esp_err_to_name(err));
        s_boot_status = 2;
        return false;
    }
    ble_hs_cfg.sync_cb = on_sync;
    ble_hs_cfg.reset_cb = on_reset;
    ble_svc_gap_init();
    ble_svc_gatt_init();
    int rc = ble_gatts_count_cfg(gatt_svcs);
    if (!rc) rc = ble_gatts_add_svcs(gatt_svcs);
    if (!rc) rc = ble_svc_gap_device_name_set(DEV_ADV_NAME);
    if (!rc) rc = ble_att_set_preferred_mtu(CONFIG_BT_NIMBLE_ATT_PREFERRED_MTU);
    if (rc) {
        ESP_LOGE(TAG, "NimBLE GATT configuration: %d", rc);
        s_boot_status = 3;
        return false;
    }
    ble_npl_event_init(&s_adv_event, ble_ads_start, NULL);
    const esp_timer_create_args_t targs = { .callback = adv_retry_cb, .name = "adv_retry" };
    if (esp_timer_create(&targs, &s_adv_retry) != ESP_OK) {
        ESP_LOGW(TAG, "advertising retry timer unavailable");
        s_adv_retry = NULL;
    }
    if (xTaskCreatePinnedToCore(ble_host_task, "nimble_host", NIMBLE_HS_STACK_SIZE, NULL,
                               configMAX_PRIORITIES - 4, NULL, NIMBLE_CORE) != pdPASS) {
        ESP_LOGE(TAG, "cannot create NimBLE host task");
        s_boot_status = 4;
        return false;
    }
    return true;
}

void app_main(void)
{
    /* 只读探测 GPIO34/GPIO5，给 (1,241) 一个真实电平；不驱动任何外部电路。 */
    board_probe_init();
    /* 不初始化/擦除原厂 NVS，也不把实验应用自动标记为有效。 */
    s_ble_ready = xEventGroupCreateStatic(&s_ble_ready_storage);
    a5_server_init(a5_notify);
    /* 先初始化 BLE：BT 控制器要在回放任务分配栈/SPI DMA 之前拿到连续内存。 */
    bool ble_ok = ble_init();
    if (ble_ok) {
        if (xEventGroupWaitBits(s_ble_ready, BIT0, pdFALSE, pdTRUE, pdMS_TO_TICKS(1500)) & BIT0)
            s_boot_status = 1;
        else if (s_adv_rc != -999)
            s_boot_status = 5;
        else
            s_boot_status = 6;   /* host 还没同步；回放里的 tick 会继续重试 */
    }
    /* 面板始终启动：BLE 状态会以“黑块个数”画在开机条纹上。 */
    playback_start();
}
