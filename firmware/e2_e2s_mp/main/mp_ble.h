#pragma once
/* mp_ble.h — Miaopaper 兼容的 BLE 传输层（GATT/广播/收包队列/通知）。
 *
 * 为什么不用 ../e2s_reader/components/ble：那个组件的协议是自定义的
 * （0x01..0x41、response=req|0x80、小端、设备名 "E2S-Reader"、私有 UUID），
 * 与 Miaopaper 的 web 工具**不兼容**。mp_ble.c 直接用 NimBLE 实现下面这套
 * 服务/特征/字节布局，唯一目的就是"原版 web tool 连上就能用"。
 *
 * 线程模型（重要）：
 *   - NimBLE host 任务里只做一件事：把写包拷进 mp_ble_req_t 队列；
 *   - **所有 mp_store_* 调用都在 app 任务里**（mp_app_ble_req），mp_ble.c 不碰 SD；
 *   - 通知（notify）由 mp_ble_notify() 在任意任务调用，内部串行化。
 *
 * 安全边界：
 *   - 广播名固定 "MPP_E2S"（web 只按 namePrefix "MPP" 过滤，web:280-285，
 *     不解析后缀）；**不含 MAC/SN/efuse 任何片段**，日志也不打。
 *   - 广播 Service Data 的 MAC6 字段填 0（字段保留是为了兼容参考的 18 B 结构，
 *     不填真实地址）。
 *
 * 只有 /sdcard/mp 会被写；编码/长度/状态码见 PORT_SPEC §7。
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* ------------------------------------------------------------------ *
 * GATT（app_att.c:63-94 的 16-bit 服务，原样保留）
 * ------------------------------------------------------------------ */
#define MP_BLE_SVC_MAIN 0x221F   /* 书 / 字库 / 锁图 / 设置 / 调试 */
#define MP_BLE_CHR_MAIN 0x331F   /* Write + WriteNoRsp + Notify */
#define MP_BLE_SVC_RXTX 0x1F10   /* 对时 */
/* Write + WriteNoRsp + Notify。**本工程不发通知**，但 NOTIFY 属性必须保留：
 * 原版 web 的 setupRxTx() 会无条件对该特征 startNotifications()，缺 notify
 * 属性会被浏览器拒绝，整条连接链进 catch 并直接断连（web:322-325、web:313）。 */
#define MP_BLE_CHR_RXTX 0x1F1F
#define MP_BLE_SVC_BATT 0x180F
#define MP_BLE_CHR_BATT 0x2A19   /* Read + Notify，1 B 百分比 */
#define MP_BLE_SVC_ENV 0x181A
#define MP_BLE_CHR_TEMP 0x2A1F   /* Read + Notify，int16 LE；本机固定 25 */
#define MP_BLE_SVC_DEVI 0x180A
#define MP_BLE_CHR_PNP 0x2A50    /* PnP ID，7 B，内容同 app_att.c:60 */

/* 扫描响应名（固定，不含身份信息）。 */
#define MP_BLE_NAME "MPP_E2S"
#define MP_BLE_NAME_LEN 7

#define MP_BLE_MTU_REQ 247          /* ble.c:34 BLE_ATT_MTU_REQ */
#define MP_BLE_PKT_MAX 244          /* MTU-3：单次 ATT 载荷上限 */
#define MP_BLE_DATA_MAX 243         /* 0x11/0x17：opcode 占 1 B 后的数据上限 */
#define MP_BLE_TITLE_MAX 19         /* 0x10 title_len 上限（ebook.c:107） */
#define MP_BLE_LOCK_BYTES 4000u     /* 0x30 只接受 4000 */
#define MP_BLE_LOCK_FLAG_OFF 0x0FF0u
#define MP_BLE_LOCK_FLAG 0xAAu
#define MP_BLE_LOCK_CHUNK 160u      /* 0x33 单块上限；实际 dlen = min(4000-off,160)。
                                     * web 固定按 160 步进（web:538/551），所以除最后
                                     * 一块外必须回满 160；需要 MTU>=166。 */
#define MP_BLE_FONT_BYTES 261696u   /* 87*94*32 */
#define MP_BLE_NOTIFY_STEP 256u     /* 0x11/0x17 每累计 256 B 回一次水位 */
#define MP_BLE_ADV_ITVL 1600        /* 1600 × 0.625 ms = 1.0 s（ble.c:266） */

/* 广播里的 0x181A Service Data（对应 ble.c:58-65 的 18 B 数组）：
 *   AD 长度字节 = 16（type 1 + UUID 2 + 13 B 载荷）
 *   载荷 13 B = MAC6(填 0) + temp2 + humi1 + batL1 + batmV2 + counter1 */
#define MP_BLE_ADV_SVC_AD_LEN 16
#define MP_BLE_ADV_SVC_PAYLOAD 13

/* 收包队列：web 的发送窗口是 min(8192, 24×243)=5832 B ≈ 24 包；
 * 队列必须留 ≥2 倍余量，否则 7.5 ms 连接间隔下必丢包（PORT_SPEC §7.4）。 */
#define MP_BLE_REQ_QUEUE 48

/* ------------------------------------------------------------------ *
 * opcode（全部来自 ebook_ble.c 的 switch：0x10..0x44 才能到应用层）
 * ------------------------------------------------------------------ */
#define MP_OP_BOOK_BEGIN 0x10
#define MP_OP_BOOK_DATA 0x11
#define MP_OP_BOOK_END 0x12
#define MP_OP_BOOK_DELETE 0x13
#define MP_OP_BOOK_COUNT 0x14
#define MP_OP_BOOK_GET 0x15
#define MP_OP_FONT_BEGIN 0x16
#define MP_OP_FONT_DATA 0x17
#define MP_OP_FONT_END 0x18
#define MP_OP_STATUS 0x19
#define MP_OP_CATALOG_COMPACT 0x1A
#define MP_OP_EBOOK_OPEN 0x20
#define MP_OP_EBOOK_CLOSE 0x21
#define MP_OP_LOCK_BEGIN 0x30
#define MP_OP_LOCK_DATA 0x31
#define MP_OP_LOCK_END 0x32
#define MP_OP_LOCK_READ 0x33
#define MP_OP_SETTINGS_READ 0x34
#define MP_OP_SETTINGS_WRITE 0x35
#define MP_OP_DEBUG_DRAW 0x40
#define MP_OP_DEBUG_EXIT 0x41
#define MP_OP_PC1_WEAK_PD 0x42   /* E2S 无 PC1：回充电检测脚(GPIO5)电平 */
#define MP_OP_PC1_STRONG_PD 0x43
#define MP_OP_PC1_HIZ 0x44

/* 0x1F1F 上的对时命令；后面 9 字节见 PORT_SPEC §7.5。 */
#define MP_OP_SET_TIME 0xDD

/* 锁屏（MP_SCREEN_LOCK）时的应用层白名单（PORT_SPEC §7.4）：
 *   放行 0x14/0x15/0x19/0x33/0x34/0x35；
 *   0x21 只 flush 进度并回 [0x21]，不切屏、不刷新；
 *   0x20/0x40/0x41/0x13 一律回 MP_ST_FAIL，不切屏、不点屏；
 *   其余（上传类 0x10..0x12、0x16..0x18、0x1A、0x30..0x32）一律回失败，
 *   否则 No_Security 的对端能在锁屏后 unlink 锁图/字库。 */

/* ------------------------------------------------------------------ *
 * 状态码（ebook_ble.c:204-215；web:939 对未知码走"未知错误"，不会崩）
 * ------------------------------------------------------------------ */
#define MP_ST_OK 0x00
#define MP_ST_SLOTS_FULL 0x01     /* 8 槽全是 DONE */
#define MP_ST_NO_SPACE 0x02       /* mp_store_space() 显示空间不足 */
#define MP_ST_BAD_PARAM 0xFD      /* 包长/长度/参数非法 */
#define MP_ST_BUSY 0xFE           /* 面板忙 / 请稍后重试 */
#define MP_ST_FAIL 0xFF           /* 通用失败 / 槽不存在 / 长度不符 / 锁屏拒绝 */

/* 0x33 请求模式下 off 越界的结束标记（原厂语义，保留） */
#define MP_ST_LOCK_END_OFF 0xFFu

/* ------------------------------------------------------------------ *
 * 时间同步（cmd_parser.c:73-75）
 * ------------------------------------------------------------------ */
typedef struct {
    uint32_t unix;    /* 本地时间当作 UTC 的秒数（固件不做时区换算） */
    uint16_t year;    /* 十进制年份的十六进制写法：2026 -> 0x07EA */
    uint8_t month;    /* 1..12 */
    uint8_t day;      /* 1..31 */
    uint8_t week;     /* 1=周一 … 7=周日（web 用 getDay()||7） */
    uint8_t hour;     /* 由 unix 换算，供界面直接用 */
    uint8_t min;
    uint8_t sec;
} mp_time_t;

/* ------------------------------------------------------------------ *
 * 收包队列（host 任务 -> app 任务）
 * ------------------------------------------------------------------ */
typedef struct {
    uint8_t op;                       /* 原始 opcode（0x10..0x44） */
    uint8_t len;                      /* op 之后的字节数，<= MP_BLE_PKT_MAX-1 */
    uint8_t data[MP_BLE_PKT_MAX - 1];
} mp_ble_req_t;

typedef struct {
    uint32_t rx;          /* 收到的 write 包数 */
    uint32_t dropped;     /* 队列满丢弃的包数（1 MB 上传压测必须为 0） */
    uint16_t mtu;         /* 当前有效 MTU */
    uint8_t connected;
    uint8_t adv;          /* 广播已开 */
} mp_ble_stats_t;

/* ------------------------------------------------------------------ *
 * 生命周期 / 连接
 * ------------------------------------------------------------------ */
/* 初始化 NimBLE、注册服务、按 mp_settings.ble_enabled 开广播。
 * 调用前 mp_settings 必须已有有效值（main.c 先调 mp_settings_defaults()，
 * mp_store_init() 成功后再覆盖为磁盘值）。
 * 失败不阻塞启动（返回非 0，界面与按键照常工作）。 */
int mp_ble_start(void);

/* 优雅断开并彻底关闭 NimBLE 与 BT 控制器（切断射频与晶振保活）；可再次 mp_ble_start()。 */
void mp_ble_stop(void);

/* 1 = BLE 协议栈运行中，0 = 已彻底关闭。 */
uint8_t mp_ble_running(void);

/* 开/关广播（设置页第 1 项、0x35）。 */
void mp_ble_adv(uint8_t on);

uint8_t mp_ble_connected(void);
uint16_t mp_ble_mtu(void);

/* 发一条 notify（<= 244 B，且 <= MTU-3）。返回 0 成功；未连接/过长返回 -1。
 * 超长直接失败而不是让协议栈截断（0x33 回读必须整块 160 B，§7.3）。
 * 0x11/0x17 的上传水位也走它（[op][written 24-bit BE]，mp_app 每跨过
 * MP_BLE_NOTIFY_STEP 发一条；web 只把它当高水位，丢一条只延后 ack）。 */
int mp_ble_notify(const void *data, uint16_t len);

/* app 任务取一条待处理请求；timeout_ms==0 表示不等待。
 * 返回 1 = 取到；0 = 超时/空。app 循环必须**先抽干队列再处理按键**。 */
uint8_t mp_ble_req_get(mp_ble_req_t *out, uint32_t timeout_ms);

/* 上传会话期间置 1：mp_ble 把连接间隔压到 7.5–10 ms，mp_app 停止时钟重绘/
 * 自动锁屏/电量采样（ebook_ble.c:25-28、app.c:99-117）。 */
void mp_ble_set_upload_active(uint8_t on);
uint8_t mp_ble_upload_active(void);

void mp_ble_stats(mp_ble_stats_t *out);

/* 事件回调（在 host 任务/连接事件里调用，只允许置标志或投递队列）：
 *   time_cb  —— 收到 0xDD 后调用，参数在回调返回后失效；
 *   conn_cb  —— 连接/断开（1/0）；
 *   wake_cb  —— 收到 BLE 包/对时/连断事件时通知 app 任务唤醒。 */
void mp_ble_set_time_cb(void (*cb)(const mp_time_t *t));
void mp_ble_set_conn_cb(void (*cb)(uint8_t connected));
void mp_ble_set_wake_cb(void (*cb)(void));

/* 广播 Service Data 与 0x180F/0x181A 特征同步（每 60 s 或电量变化时调）。 */
void mp_ble_publish_battery(uint8_t percent, uint16_t mv);
void mp_ble_publish_temp(int16_t celsius);
