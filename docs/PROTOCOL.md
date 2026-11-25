# PROTOCOL: e2s_mp 的 Miaopaper 兼容 BLE 协议

目标是让原版 MiaoPaper.html 连上就能用。服务、特征 UUID、opcode、字节序、
状态码和应答形状按参考实现对齐（`refs/Miaopaper/Firmware/src/ebook_ble.c`
与 `MiaoPaper.html`）。本工程不新增 op。

本文按当前 `main/mp_ble.c` 和 `main/mp_app.c` 写。未上真机，未跑原版
web tool。已知偏差见 §9，验证范围见 §11。

---

## 1. 连接与 GATT

### 1.1 服务与特征

| 用途 | Service | Characteristic | 属性 |
|---|---|---|---|
| 书、字库、锁图、设置、调试 | `0x221F` | `0x331F` | Write + WriteNoRsp + Notify |
| 对时 | `0x1F10` | `0x1F1F` | Write + WriteNoRsp + Notify |
| 电池 | `0x180F` | `0x2A19` | Read + Notify（1 B 百分比） |
| 温度 | `0x181A` | `0x2A1F` | Read + Notify（int16 LE，固定 25） |
| 设备信息 | `0x180A` | `0x2A50` | Read（PnP 7 B） |

- `0x1F1F` 必须保留 NOTIFY 属性。原版 web 的 `setupRxTx()` 无条件调用
  `startNotifications()`，缺少 notify 会被浏览器拒绝并断开连接。本工程
  不在该特征上发通知。
- Device Name 为 `ESL`，照抄参考实现。扫描响应名为 `MPP_E2S`，web 只按
  `namePrefix:'MPP'` 过滤。
- `0x2A1F` 是 int16 LE。`0x2A50` 是 7 B PnP ID。
- 没有 128-bit EPD 推屏服务。原版 web 不使用它。

### 1.2 广播

- 广播数据为 Flags（3 B）加 `0x181A` Service Data（AD 长度 16）。
- 13 B 载荷：`MAC6(全 0) + temp2(大端) + humi1(0) + batL1 + batmV2(大端)
  + counter1`。counter 每次刷新加 1。
- 广播间隔 1.0 s，即 `1600 × 0.625 ms`。
- MAC 字段恒为 0。设备不广播、不打印 MAC、SN、efuse 或 NVS。
- 扫描响应放名字，广播数据里不放名字。

### 1.3 MTU 与连接参数

| 项 | 值 |
|---|---|
| preferred MTU | 247 |
| 设备主动交换 | 连接后 `ble_gattc_exchange_mtu()`；上传开始若 MTU < 100 再试一次 |
| 连接间隔 | 6..8（7.5..10 ms），latency 0，timeout 2000（20 s） |
| notify 上限 | `min(244, MTU-3)`，超长直接失败 |
| `0x33` 要求 | MTU ≥ 166（3 + 160 B） |

`ROLE_CENTRAL` 和 `GATT_CLIENT` 在 `sdkconfig.defaults:32-33` 打开。
`build/config/sdkconfig.h` 中为 1，ELF 含 `ble_gattc_exchange_mtu`。
中央不主动 requestMTU 时，设备仍会主动谈一次。

### 1.4 安全

使用 `No_Security`，不配对、不加密（`sm_io_cap=NO_IO`、`sm_bonding=0`、
`sm_mitm=0`）。锁屏时的应用层白名单因此是必需的安全边界，不能依赖链路加密。

### 1.5 发送队列

- app 主循环每轮先抽干 BLE 队列，再取按键。
- 队列深 48（`MP_BLE_REQ_QUEUE`），约为原版 web 5832 B 发送窗口的 2 倍。
- 队列满时 `dropped++`，写回包路径返回 `ATT INSUFFICIENT_RES`。
  1 MB 上传的目标是 `dropped==0`，尚未在真机验证。
- 断连时 `xQueueReset` 并回调 `conn_cb(0)`，上一会话的残留包不会进入下一会话。

---

## 2. 帧、字节序与长度

- 写包首字节是 opcode。只有 `0x10..0x44` 会到应用层，其余忽略。
  参考固件里其他首字节会进入 OTA 分支，本工程没有 OTA。
- 多字节一律大端（u16、u24、u32）。
- `mp_ble_req_t.len` 是 opcode 之后的字节数。参考实现的 `payload_len`
  含 opcode，换算为 `payload_len = 1 + req->len`。
- 单次写载荷上限 244 B（MTU-3）。`0x11` 和 `0x17` 的数据部分上限 243 B。

---

## 3. 状态码

| 值 | 名称 | 含义 |
|---|---|---|
| `0x00` | OK | 成功 |
| `0x01` | SLOTS_FULL | 8 槽全是 DONE |
| `0x02` | NO_SPACE | 空间不足（free < total + 64 KiB） |
| `0xFD` | BAD_PARAM | 包长、长度或参数非法 |
| `0xFE` | BUSY | 面板忙、已有上传会话或锁屏拒绝 |
| `0xFF` | FAIL | 通用失败、槽不存在、长度不符或锁屏拒绝 |

`0x10` 的失败应答一律 3 字节：`[0x10][st][0xFF]`。
`0x15` 的失败应答是 `[0x15][idx][0xFF]`，索引在前。

---

## 4. op 表

| op | 名称 | 请求（op 之后） | 应答 | 实现要点 |
|---|---|---|---|---|
| `0x10` | 书-开始 | `[title_len][title…][total u24][enc]` | `[0x10][st][slot]` | `title_len ≥ 2`；`total>0`；`title_len>19` 截到 19 且不切断双字节；8 槽满回 `0x01`；忙回 `0xFE`；空间不足回 `0x02`；存储失败回 `0xFF`；参数错回 `0xFD`。失败也回 3 字节，slot 为 `0xFF` |
| `0x11` | 书-数据 | `[raw 1..243]` | 每累计 256 B：`[0x11][written u24]` | 无会话时回 `written=0`；没有 offset，按 `off=written` 顺序写；越界或写失败立即 `upload_abort()`，随后回 `written=0`；收到数据刷新 60 s 超时 |
| `0x12` | 书-结束 | `[]` | `[0x12][st][slot][written u24]` | 必须 `written==total` 且 `mp_store_finish()` 成功（含 `fstat` 大小核对），否则回 `0xFF` 并 abort；成功置 DONE |
| `0x13` | 删书 | `[idx]` | `[0x13][st]` | 删当前打开的书时先 flush 和关书；在阅读屏则回 CLOCK；成功后 `mp_progress_save(idx,0)`；上传该槽时回 `0xFE`；失败回 `0xFF` |
| `0x14` | 书数 | `[]` | `[0x14][count]` | 只数 DONE；存储不可用回 0 |
| `0x15` | 查单本 | `[idx]` | 3 条：`[0x15][idx][0x00][title 0..11]`（15 B）、`[0x15][idx][0x01][title 12..19]`（11 B）、`[0x15][idx][0x02][len u24][enc]`（7 B） | 三条背靠背，不插延时；空槽、存储不可用或参数错回 `[0x15][idx][0xFF]`；实现未使用 `0xFE` |
| `0x16` | 字库-开始 | `[total u24]` | `[0x16][st]` | 开新会话；忙回 `0xFE`；存储失败回 `0xFF`；`0x18` 以实际收到 261,696 B 为准 |
| `0x17` | 字库-数据 | `[raw]` | 每 256 B `[0x17][written u24]` | 与 `0x11` 相同 |
| `0x18` | 字库-结束 | `[]` | `[0x18][st]` | `written == 261696` 且 `font.bin` 大小为 261,696，否则回 `0xFF` |
| `0x19` | 状态 | `[]` | `[0x19][font_ok][book_cnt][lock_ok][att_mtu]` | `font_ok` 来自 `mp_store_font_ok()`，内嵌 HZK16 或上传的 261,696 B 文件都算有字库；`book_cnt` 是真实 DONE 数；`lock_ok` 是 0xAA 标志；`att_mtu` 夹到 255。连接后 2 s 主动发一次 |
| `0x1A` | 目录压缩 | `[]` | `[0x1A][st]` | 回收非 DONE、非当前会话的槽；上传中回 `0xFE`；存储失败回 `0xFF` |
| `0x20` | 打开书 | `[idx]` | `[0x20][st]` | DEBUG 或上传中回 `0xFE`；存储、idx 或坏书失败回 `0xFF` 并保持当前屏；成功切到 READ |
| `0x21` | 关闭书 | `[]` | `[0x21]`（1 B） | 保存进度、清 `pending_ok`、回 CLOCK、全刷；DEBUG 下只回包；锁屏下只 flush 进度并回包，不切屏不刷新 |
| `0x30` | 锁图-开始 | `[total u24]`，必须 4000 | `[0x30][st]` | 非 4000 回 `0xFD`；忙回 `0xFE`；写 `lock.tmp`，旧 `lock.bin` 保留到 `0x32` 发布；存储失败回 `0xFF` |
| `0x31` | 锁图-数据 | `[off u16][data]`，`off+len ≤ 4000` | `[0x31][st]` | 允许乱序和覆盖；写 `lock.tmp` 并记录 4000-bit 覆盖位图；越界回 `0xFF` |
| `0x32` | 锁图-结束 | `[]` | `[0x32][st]` | 4000 B 必须全部收到；`fsync` 后写 `0xAA` @0x0FF0，再 `rename(lock.tmp, lock.bin)`；不完整回 `0xFF`，旧图不动；成功后重载 RAM 缓存 |
| `0x33` | 回读锁图 | `[off u16]`（空请求为 legacy，不支持） | `[0x33][off u16][data dlen]`，`dlen = min(4000-off, 160)` | 空请求回 `[0x33][0xFE]`；`off ≥ 4000` 回 `[0x33][0xFF][0xFF]`；MTU<166 回 `[0x33][0xFE]`；无图也回全 0 块；除最后一块外必须回满 160 |
| `0x34` | 读设置 | `[]` | `[0x34][ble][sleep_idx][major][minor][patch]` | 6 B；版本取 `READER_VERSION_*` |
| `0x35` | 写设置 | `[ble][sleep_idx]` | `[0x35][st]` | `sleep_idx` 越界夹到默认 2（180 s）；立即存盘并调用 `mp_ble_adv()` |
| `0x40` | 调试画线 | `[y u16]` | `[0x40][st][y]` | `y` 夹到 ≤121；画全宽横线；进入 DEBUG（30 s，`0x40` 续期）；锁屏时被白名单拒绝 |
| `0x41` | 退出调试 | `[]` | `[0x41][0]` | 清 DEBUG，回 CLOCK 并全刷；锁屏时被拒绝 |
| `0x42/0x43/0x44` | PC1 测试 | `[]` | `[op][level]` | E2S 没有 PC1，回充电检测脚 GPIO5 的电平（1=高，0=低），不驱动也不改配置。原版 web 的“PC1 充电状态测试”页会发这三个 op |
| `0xDD` | 对时 | `[0xDD][unix32][year u16][month][day][week]`（写在 `0x1F1F`） | 无 | 见 §7；只接受 10 B；month/day 非法丢弃；week 越界夹到 1 |

补充：

- `0x11` 和 `0x17` 的应答是累计已写字节的高水位，不是本包 ack。
  越界或写失败触发 abort 后回 `written=0`，高水位已经清零。
- `0x15` 的 title 是原始目录字节（20 B 字段，最多 19 B 加 NUL），不保证 UTF-8。
- `0x19` 的 `book_cnt` 为 0..8，原版 web 用它显示书数。
- `0x42..0x44` 在 `mp_ble.c` 的写回调里就地回包，不进 app 队列。
  app 里也有同样语义的分支，正常路径不会收到。

---

## 5. 上传状态机与流控

```
IDLE --0x10--> BOOK(校验 title/total、取槽、建 b<slot>.bin)
     --0x16--> FONT(261696 B)
     --0x30--> LOCK(4000 B)
任一 ACTIVE --0x11/0x17/0x31--> 写数据
任一 ACTIVE --0x12/0x18/0x32--> 校验，发布 DONE 或 0xAA，回到 IDLE
任一 ACTIVE --断连或 60 s 无数据--> abort，回到 IDLE
```

- 同一时刻只允许一个上传会话。`0x10`、`0x16`、`0x30` 在别的会话中回 `0xFE`。
- `0x11` 没有偏移字段。设备只能按 `off = written` 顺序写。丢一包不会表现为
  `off != written`，而是最终 `written < total`。兜底是 `0x12` 的
  `written == total` 和 `fstat` 大小核对。
- 主机不会重传。短上传必须 abort，`0x12` 回 `0xFF`，不能把短书置为 DONE。
- 60 s 无数据（`last_pkt_ms`）自动 abort。
- 队列深 48。上传期间普通按键和场景路径不重绘、不采样电量、不自动锁屏。
  BLE `0x40` 是例外，它直接 `debug_draw()` 出帧。

---

## 6. 锁屏白名单

`MP_SCREEN_LOCK` 时（`mp_app.c:2083-2113`）：

| op | 行为 |
|---|---|
| `0x14 / 0x15 / 0x19 / 0x33 / 0x34 / 0x35` | 正常处理，只读或设置，不切屏 |
| `0x21` | 只调用 `mp_app_flush_progress()`，回 `[0x21]`，不切屏、不刷新 |
| `0x13 / 0x20 / 0x40 / 0x41` | 回 `0xFF`，不切屏、不点屏 |
| 上传类（`0x10..0x12`、`0x16..0x18`、`0x1A`、`0x30..0x32`） | 失败。`0x10` 回 3 字节，`0x11/0x17` 回 `written=0`，`0x12` 回 6 字节失败，其余回 `[op][0xFF]` |

理由是 GATT 为 `No_Security`，不能让任意对端在锁屏后 unlink 锁图或字库、
压缩目录，或开始上传会话。

两个例外在传输层就地处理，不经过白名单：

- `0x42/0x43/0x44` 在 `0x331F` 写回调里直接回 GPIO5 电平，锁屏时仍可用。
- `0xDD` 在 `0x1F1F` 上直接进时间回调，锁屏时仍会更新时间基准，但不重绘。

---

## 7. 对时与走时

请求写在 `0x1F1F`，共 10 B：

```
[0xDD][unix u32 BE][year u16 BE][month][day][week]
```

- `year` 是十进制年份的十六进制写法（2026 写作 `0x07EA`），不要按年号解释。
- `week`：1=周一，7=周日。
- `unix` 是本地时间当 UTC 的秒数。固件不做时区换算。
- 设备记录 `time_base_unix` 和 `time_base_ms`，每秒用单调毫秒重算 `hour/min`。
  跨 24:00 时按月份天数表和闰年进位 `day/month`，`week = week%7+1`。
  不重算 `year`，每次连接 web 都会重发。
- 未同步时默认 `2026-01-01 00:00:00`（周四），`time_valid=1`。
  上电后从默认值开始走，屏幕不会永远停在 00:00。
- 其他首字节（`0xE2`、`0xED`、`0xF3` 等）一律忽略。非法 month/day 丢弃。
- `0xDD` 无应答。

---

## 8. 通知

| 通知 | 时机 |
|---|---|
| `0x11` 和 `0x17` 水位 | 每累计 256 B |
| `0x12/0x18/0x30..0x35` 应答 | 处理完立即 |
| `0x19` | 每次请求；连接后 2 s 主动一次 |
| `0x15` | 三条背靠背，不插延时 |
| `0x2A19` 电量 | app 每秒调用 publish；`mp_ble` 按“值变化或距上次 60 s”节流 |
| `0x2A1F` 温度 | 同上，固定 25 |
| 广播 Service Data | 电量变化时刷新，没有独立的 60 s 定时器 |

原版 web 不订阅 `0x2A19` 和 `0x2A1F`，所以这些通知只影响第三方客户端。

---

## 9. 已知限制与偏差

| 编号 | 现象 | 证据与影响 |
|---|---|---|
| P1 | `0x33` 需要 MTU≥166。链路仍为 23 时设备回 `0xFE`，主机应显示“该链路不支持预览”。上传和阅读不受影响 | `mp_app.c:1998-2001`；Windows Chrome 若中央不协商，预览不可用 |
| P5 | `0x10` 的 `title_len` 必须 ≥2，1 字节书名回 `0xFD`。这是参考行为，不是 bug | `mp_app.c:1618-1622` |
| P6 | `0x11` 没有 offset，主机不重传，短上传只能 abort | `mp_app.c:1687-1719` |
| P7 | `0x15` 没有 `0xFE` 忙状态，失败统一回 `[0x15][idx][0xFF]` | `mp_app.c:1807-1848` |
| P8 | 锁屏下 `0x13/0x20/0x40/0x41` 与全部上传类都回失败，不只挡切屏 | `mp_app.c:2083-2113` |
| P9 | 没有 OTA 或固件下载 op，`0x10..0x44` 之外的首字节被忽略 | `mp_ble.c:407-468` |
| P10 | 设备信息与广播不含 MAC、SN、efuse；广播 MAC 字段恒 0 | `mp_ble.c:192-231`、`mp_ble.h:1-30` |
| P11 | `0x40` 在 `uploading==1` 时没有专门互斥，仍会进入 DEBUG | `mp_app.c:2209-2219`；未上真机 |

早期记录中的 P2（`0x2A19/0x2A1F` 没有通知）、P3（`0x32` 不校验 4000 B
收满）、P4（`0x21` 不清 `pending_ok`）已修复，不再列为限制。

---

## 10. 兼容性验收点

按原版 web 的调用顺序：

1. `filters=[{namePrefix:'MPP'}]` 能扫到 `MPP_E2S`。
2. `0x221F/0x331F` 和 `0x1F10/0x1F1F` 都能 `getPrimaryService` 并
   `startNotifications`。
3. 连上 2 s 内收到 `0xDD` 对时、`0x19`、`0x14` 和逐槽 `0x15` 的回包。
4. 上传一本 1 MB txt：`0x10 → 0x11×N → 0x12`，`dropped==0`，
   `written` 单调到 total，成功后列表出现新书。
5. `0x20` 打开书，设备进 READ；`0x21` 回 CLOCK 并保存进度。
6. `0x33` 拿到 25 块 ×160 B（要求 MTU≥166）；无图也回全 0 块。
7. `0x34` 读设置，`0x35` 开关蓝牙，广播随之开关。
8. 锁屏时 `0x20/0x40/0x41/0x13` 全部失败，屏幕不变；`0x14/0x19` 正常；
   `0x21` 只回包不切屏。
9. `0x42/0x43/0x44` 回 GPIO5 电平，不改变引脚状态。
10. `0xDD` 跨分钟和跨 24:00 时日期、星期正确进位。

---

## 11. 验证状态

本次实际运行：

- `./tools/build.sh build` 输出 `Project build complete`，镜像 936,800 B
  （0xe4b60）。ELF 中有 `ble_gattc_exchange_mtu` 和 `ble_att_clt_tx_mtu`，
  说明 MTU 交换分支已编译。
- `./sim/run.sh --check` 通过 25 帧、289 条文案。模拟器不编译 `mp_ble.c`，
  只验证 UI 侧。

未运行：

- 真机 BLE 连接、原版 web tool、1 MB 上传、MTU 协商、`0x33` 回读、
  `0xDD` 走时、锁屏白名单实测、`dropped==0`。

P1、P5..P11 来自静态代码阅读，不是实测结论。
