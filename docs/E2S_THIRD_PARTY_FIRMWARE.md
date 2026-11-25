# E2S 第三方固件：纯软件经验总结

> 目标：为后续在 Paperang E2S（ESP32-S3）上做第三方固件打底。
> 本文只写软件层面：启动链、OTA/恢复、BLE、墨水屏、回放、按键、构建配置、调试方法。
> 不涉及拆机/夹具/供电等硬件操作。

---

## 0. 先记住这 10 条

1. **只写 app 槽**（ota_0），原厂 ota_1 永远不动；bootloader、分区表、NVS 不写。
2. **otadata 自己算**，不要假设现有两条记录一致；切回原厂的算法必须能容忍重复/损坏记录。
3. **`ble_gap_adv_start()` 返回 0 ≠ 射频真的在发**。必须用独立扫描器（两个）+ 按 MAC 直连验证；面板上再画一个状态码。
4. **NimBLE 的 GAP 启停只在 host 任务里做**；其他任务（回放/按键）只投递事件。
5. **`app_main` 不能因为 BLE 初始化失败就 return**；面板/回放要独立于 BLE 启动。
6. **墨水屏冷启动第一帧必须全刷**；全用局部刷新会在冷启动时整屏空白，极易误判“设备没启动”。
7. 实测**全刷完成 1.312 s、局刷完成 454 ms**；之前记的 28 s/126 ms 是等待策略的返回值，不是面板完成时间。**2 fps（500 ms）是上限**，6 fps 必撕裂。
8. **大块帧数据不要嵌进 app**；放 flash 空白区（0x530000），app 从 1.13 MB 降到 470 KB。
9. 自定义固件**冷启动的射频工作状态**和原厂不同：原厂从 NVS 的 `phy` 命名空间取出厂校准，自定义固件默认每次开机做全量校准。低电量/冷启动下后者可能“host 说广播成功、实际不发波”。
10. **面板就是日志**：启动阶段码、BLE 码、帧区状态、按键掩码都放 diag，别等 UART。

---

## 1. 平台与启动链（软件视角）

- SoC：ESP32-S3；IDF：v5.4.4；flash：8 MB（GD25Q64，JEDEC `C8 40 17`）。
- App 镜像头：chip id 9、DIO 80 MHz、8 MB、min rev 0、max rev 99；esptool 的 checksum / appended SHA-256 必须有效。
- 运行时分区表来自 flash，不是编译时的表：

| 分区 | 偏移 | 大小 |
|---|---|---|
| nvs | 0x9000 | 0x6000 |
| otadata | 0xF000 | 0x2000 |
| phy_init | 0x11000 | 0x1000 |
| ota_0 | 0x20000 | 0x1F0000 (2 MB) |
| ota_1 | 0x210000 | 0x1F0000 (2 MB) |

- bootloader 在 0x0，DIO 80 MHz，8 MB。
- **本地构建用的分区表（SINGLE_APP_LARGE 等）只是编译约束，绝不要刷到设备**；运行时看 flash 里的表。
- app 给自己留余量：< 1 MB 最舒服（当前 470 KB），ota_0 上限 2 MB。
- 不要用 `ESP_ERROR_CHECK` / `assert` 卡住可选路径；宁可降级运行，也要让面板和 BLE 起来。

### otadata 细节（必须自己实现的部分）

`esp_ota_select_entry_t`（32 B）：

```c
struct {
    uint32_t ota_seq;        // +0
    uint8_t  seq_label[20];  // +4
    uint32_t ota_state;      // +24
    uint32_t crc;            // +28
};
```

- CRC 只覆盖 `ota_seq` 4 字节：`esp_rom_crc32_le(UINT32_MAX, &seq, 4)`，
  等价于 Python `zlib.crc32(struct.pack('<I', seq), 0xFFFFFFFF)`。
- 槽位映射：`slot = (seq - 1) % ota_count`；ota_count = 2。
- `ota_state = 0xFFFFFFFF`（UNDEFINED）是合法可启动状态；只有 rollback 打开时 NEW/PENDING_VERIFY 才有特殊含义。
- 选槽：两条记录里取 **合法 CRC 且 seq 最大** 的那条；两条都非法/0xFFFFFFFF 时走 factory/遍历分区。

---

## 2. OTA 与“永远能回原厂”（纯软件）

### 原厂 OTA 协议

- A5 服务：`0000ff00-c243-40b9-ac41-a10ba3ad5d1a`，FF01 notify / FF02 write / FF03 ready。
- OTA：`(2,1)` start、`(2,3)` chunks、`(2,2)` end、`(1,19)` trigger。
- 原厂固件写入**非运行槽**，再写 otadata；断电/失败时原厂槽仍可启动。
- **自定义固件不要实现 OTA 写入**（安全考虑）：升级路径统一为
  `自定义 → (1,240) 回原厂 → 原厂 OTA 写新自定义固件 → 重启`。

### 切回原厂的稳健算法

不要依赖现有 otadata 两条记录“成对一致”（原厂 OTA 产生过 43/43 重复记录，直接把旧逻辑打挂）。

1. 读出两条原始记录（不管状态）。
2. 找 `max_seq = max(合法 seq)`；新 `seq = max_seq + 1`，递增到 `(seq-1)%2 == stock_slot`。
3. 擦除**不持有最高 seq 的那个扇区**（保留另一条作回退），写新记录：
   `{seq, state=UNDEFINED, crc}`，回读逐字节校验。
4. 校验目标槽是有效 app：magic `0xE9`、app desc magic `0xABCD5432`、SHA-256、分区不重叠。
5. `esp_ota_get_boot_partition() == stock_slot` 后再用 `esp_timer` 延时 3 s `esp_restart()`。
6. A5 `(1,240)` 的返回码：`01` = 已选原厂槽、即将重启；`02` = 预检失败/无有效原厂槽；`03` = 写失败。

---

## 3. BLE / NimBLE（坑最多）

### 结构

- `(1,7)` version、`(1,241)` diag、`(1,240)` 切原厂、`(1,242)` 手动全刷、`(3,1)/(3,2)` 只读 flash。
- MTU：247（自定义）/185（原厂实协商）。
- 广播：flags + 12 B 名字放 adv（17 B ≤ 31），128-bit UUID 放 scan response（18 B ≤ 31）。

### 正确的初始化/启停模型

```c
app_main:
  a5_server_init();
  ble_init();                 // nimble_port_init + GATT + host task(core0, prio MAX-4)
  wait ≤1.5 s for adv bit;    // 只用于状态显示，不阻塞回放
  playback_start();           // 面板/回放独立启动

host task:
  nimble_port_run();          // 所有 GAP 调用都在这里
```

- `on_sync`：`ble_hs_util_ensure_addr(0)` + `ble_hs_id_infer_auto(0, &type)`（或直接用 `BLE_OWN_ADDR_PUBLIC`）。
- 广播启动/停止只放在 host 任务的 event callback 里；其他任务只调用 `ble_npl_eventq_put()`（线程安全）。
- 启动失败：记录 `s_adv_rc`，用 `esp_timer` 500 ms 后 **只投事件**重试；不要在失败分支里立刻重新入队（会自旋）。
- 断连/ADV_COMPLETE：host 任务内 `clear_connection()` + 重新入队。
- 回放任务的 `ble_service_tick()` 只做只读检查（`ble_hs_synced`、`ble_gap_conn_find`、`ble_gap_adv_active`）+ 入队，**绝不直接 `ble_gap_adv_stop/start`**。
- 待机/播放切换：`ble_set_advertising(bool)` 只改标志 + 入队；host 任务按标志启停；tick 尊重该标志。

### “host 说广播成功、实际收不到包”的排查顺序

1. 两个独立扫描器（Bleak + `bluetoothctl`）按 **MAC / 名字 / 128-bit 服务 UUID** 三种条件匹配；
2. 按 MAC 直连（Bypass 扫描过滤）；
3. 面板画状态码：`1=adv 已起`、`2=nimble_port_init 失败`、`3=GATT 配置失败`、`4=host 任务创建失败`、`5=adv_start 失败`、`6=host 1.5 s 内未同步`；
4. 排除“正在播放/面板刷新导致停广播”的变量：让设备停在待机（只广播）状态观察；
5. 用原厂固件做对照：原厂能广播 → 射频/控制器路径没问题，差异在自定义固件的初始化配置。

### PHY 校准：软件层的真正差异

- `CONFIG_ESP_PHY_CALIBRATION_AND_DATA_STORAGE=n` 时，IDF 在 `phy_init.c` 里强制
  `register_chipv7_phy(..., PHY_RF_CAL_FULL)`，**每次启动做全量校准**。
- 原厂固件用 `CONFIG_ESP_PHY_CALIBRATION_AND_DATA_STORAGE=y`：从 NVS 的 `phy`
  命名空间读 `cal_data`（1904 B）+ `cal_mac`（6 B）+ `cal_version`，只做局部校准。
- 实测现象：自定义固件冷启动 → host 广播成功但射频不发波；先启动原厂、再软重启进自定义 → 广播正常。
  软件层差异就是“全量校准 vs 出厂校准数据”。
- 后续可选方案（按风险从低到高）：
  1. **只读解析 NVS**：直接读 0x9000 起的 NVS 分区，取出 `phy/cal_data`，不调用 `nvs_flash_init`，也绝不写 NVS；
  2. 把本机的出厂校准 blob 在构建时嵌进 app，作为兜底；
  3. `CONFIG_ESP_PHY_CALIBRATION_AND_DATA_STORAGE=y` + `nvs_flash_init()`：
     标准做法，但要注意 NVS 版本迁移/校验失败时 NVS 层可能擦除重建，**不能无确认地做**。
- **红线**：不初始化/擦除原厂 NVS；`sn`/`key`/`bind`/`cal_*` 是设备身份，不打印、不外传。

---

## 4. 墨水屏驱动与回放

### 面板

- 250×122，1 bpp，行宽 32 B，MSB 在前，`1=白`。
- 引脚：CS 40 / DC 39 / RST 48 / BUSY 47 / MOSI 41 / SCLK 42；SPI3 3-wire half-duplex 10 MHz。
- 面板族：复位后 BUSY=0 → BOE；BUSY=1 → 读 `0x70` ID，按 ID 分 OED/GD。
- 全刷（BOE）：`0x12` → 参数/LUT → `0x24`+`0x26` 写两遍 RAM → `0x22 0xC7` → `0x20`。
- 局刷：`0x24` 当前帧 → `0x22 0xFC` → `0x20` → 等待 → `0x26` 回写同一帧（给下一帧做差分基准）。
- **实测时间（2026-11-24 严格计时，BOE，两次结果完全一致）**：
  - 全刷总耗时 **1,312 ms**：`0x12` 后 2 ms、配置后 0 ms、写帧后 0 ms、`0x20` 后 **1,266 ms**；
  - 局刷 `0x20` 后 **454 ms** 回到空闲（BUSY 确实拉高过，`pb=1`）；
  - 旧值 `fms=28007 ms` / `pm≈126 ms` 是旧 `wait_reaction` 的返回值：0x20 时 BUSY 已经是 1，
    函数却等它“回到起始电平 1”，面板实际回到 0 后一直保持 0，所以白等满 20 s；
    `b1/b2` 两个阶段面板根本没拉 BUSY，又各白等 1.5 s。28 s 完全是自等待，不是面板慢。
  - 正常局刷在 126 ms 就把 `0x26` 写回去了，而面板 454 ms 才完成——这次回写很可能被忽略，
    是残影的上游原因；改成“等 BUSY 真正空闲再写 0x26”才稳。
- **冷启动第一帧必须全刷**：全用局刷时，RAM1/RAM2 没有正确的初始基准，实测整屏不显示。
  我们的待机画面（Hello Paperang + 状态行）就是一次全刷，同时完成面板初始化。

### 帧率

- 6 fps = 166 ms/帧，远小于 454 ms 的局刷完成时间 → 画面撕裂/破碎。
- 2 fps = 500 ms/帧，留 46 ms 余量，是这台面板的安全上限；用 6 fps 的帧数据每 3 帧取 1 帧，
  正好 438 帧 / 219 s。
- 3 fps = 333 ms/帧 < 454 ms，必撕裂；4 fps 更不可能。**不要再用“3–4 fps”作为目标**。

### 帧数据格式（BAPL）

```
0  "BAPL"
4  u8  version=1
5  u8  flags
6  u16 fps
8  u16 width (250)
10 u16 height (122)
12 u32 frame_count
16 frame_count * 8B: { u32 payload_offset; u8 flags; u8[3] reserved; }
... payload: 每帧 RLE，LEB128 游程，0/1 交替且从 0(黑) 开始
```

- 解码输出 32×122×8 = 31,232 bit（含每行 6 bit 填充）；解码器在填满后停止，**尾部 0xFF 填充不会被读取**。
- 单帧 RLE 最大 1183 B，平均 492 B。

### 帧数据放哪里

- 嵌进 app：657 KB 帧 → app 1.13 MB（虽然 2 MB 槽放得下，但 OTA 慢、镜像大）。
- 放 flash 空白区：`0x530000`（原厂字体区 0x480000–0x528884 之后，dump 里全 0xFF）。
  - app 缩到 470 KB；
  - 启动时读 16 B 头 + `count*8` 表（堆，10.5 KB）+ 单帧缓冲（2 KB）；
  - 用 `esp_flash_read()`，长度向上取 4 字节；最后一帧多读 2048 B，解码器会自然停止；
  - 帧区头校验失败时不要崩：停在待机画面继续广播（`fr=no_data`）。

### 4.5 实测结果（2026-11-24，BOE，两次一致）

严格计时在真机上跑通，`(1,243)/(1,244)` 两次结果逐位相同：

| 项目 | 实测结果 | 结论 |
|---|---|---|
| 全刷真实耗时 | `full=1312 ms`，`ph=2/0/0/1266`：0x12 后 2 ms、配置后 0 ms、写帧后 0 ms、0x20 后 1266 ms | 面板全刷完成约 **1.31 s**；28 s 是旧 `wait_reaction` 的自等待 |
| 局刷真实耗时 | `part=454 ms`，`pb=1`，`ev=+454:0` | 面板局刷完成约 **454 ms** |
| BUSY 极性（BOE） | `bm=9`（0x12 和 0x20 阶段见过 BUSY=1），0x20 后 `+1266:0` | **高=忙，低=空闲**，与反汇编一致 |
| GPIO34/GPIO5 | 启动时和待机时都是 `g34=1 g5=1`（电池 100%） | 静态电平已知；语义未确认 |
| 按键 | `kc` 最终 `2,2,2,2,2`，五个脚各自被按下，按住 219～1399 ms | 5 个键都有效、互相独立 |
| OED `wait_level(1)` | 未执行（`fam=1 p47=0`） | 本机无法验证 |

原始结果（两次逐位一致）：

```
tm=1 fam=1 full=1312 ft=0 bm=9 ph=2/0/0/1266 ev=+1266:0 part=454 pt=0 pb=1 ev=+454:0
```

旧值为什么错：

- 0x20 时 BUSY 已经被拉高，`wait_reaction()` 记录的 base=1，然后它等“变低再回到 1”；
  面板完成后一直是 0，于是白等满 20 s。`0x12` 后的 b0 同理白等 5 s。
- `b1/b2` 两个阶段面板根本没拉 BUSY，又各白等 1.5 s。
- 合计 5+1.5+1.5+20=28 s，正好是 `fms=28007`。**面板从来只有 1.3 s。**

由此产生的两个直接影响：

1. 正常路径的局刷在 ~126 ms 就写 `0x26`，但面板 454 ms 才完成；这次回写很可能被忽略，
   是残影/“该刷不刷”的上游原因。应改成“等 BUSY 真正空闲再写 0x26”。
2. 待机/停止播放时的全刷白等 28 s；改成“等 BUSY 低电平（上限 5 s）”后，开机和停止
   都能在 ~1.5 s 内回到待机。

仍未闭环：

- **按键丝印 → GPIO 的具体标签**：计数证明 5 个脚都通且独立，但没有做“只按某个丝印键”
  的单键标注；需要再刷一次带 `(1,241)` 的固件，按指定键逐个记录。
- **GPIO5 语义**：只读到 1（电池 100%），没有做“放电后插电”的对照，不能确认“低=充电”。
- **GPIO34 的 50 ms 低脉冲**：没有 SD 挂载实验，仍是反汇编推断。
- **OED 分支**：本机 BOE，`wait_level(1)` 永远走不到，只能等 OED 实机。

测量代码已经加进 `firmware/badapple`，不影响正常回放：

- `(1,243)`：触发一次“严格全刷 + 严格局刷”。全刷不再用 5/1.5/1.5/20 s 超时，而是
  每一步都 1 ms 轮询 BUSY、等它真正回到低电平；总时长和各阶段耗时都记录。
- `(1,244)`：读回结果。
  - `full=` 从 0x12 到 0x20 后空闲的总毫秒数；
  - `ph=a/b/c/d` 四个阶段（0x12 后、配置后、写帧后、0x20 后）各自到空闲的毫秒数；
  - `bm=` 四个阶段是否见过 BUSY=1 的位图（用来确认“高=忙”在这台机器上成立）；
  - `ft=` 哪些阶段等到超时（正常应为 0）；
  - `ev=+毫秒:电平,...` 是 0x20 后 BUSY 的每次跳变；
  - `part=` / `pt=` / `pb=` / `ev=` 是局刷的同一组数据。
- `(1,241)` 新增：`kc=`（GPIO2/4/6/7/8 各自的按下次数）+ `last=脚/按住毫秒`，
  以及 `g34=`、`g5=` 的实时电平。

闭环步骤（一次刷机 + 几次按键）：

1. 刷入带测量钩子的 app（仍走原厂 OTA，只写非运行槽）；
2. 待机状态跑 `python3 tools/e2s_ble_flash.py measure`（自动发 `(1,243)`、轮询
   `(1,244)`、再打印一次 `(1,241)`）；严格路径本身约 2 s，`measure` 最多等 120 s；
3. 每个物理键各短按一次，每按一次跑 `python3 tools/e2s_ble_flash.py diag`：
   `kc` 只有一个位置增加，就得到“丝印键 → GPIO”的映射；`last=` 同时给出按住时长；
4. 插/拔 USB 各读一次 `diag`：`g5` 跟着变才能确认“低=充电”，不变则说明
   该脚不是充电检测或本机没有这条电路；
5. `g34` 只能给出上电后的静态电平。要确认“低 50 ms 是上电还是复位”，还需要
   有 SD 卡的机器做挂载实验；本机若无 SD 座，这一项只能停在“电平已知、语义未知”。

**OED 分支的 `wait_level(1)` 在这台机器上无法闭环**：面板族是 BOE（`fam=1 p47=0`），
OED 路径永远不会执行。将来如果换到 OED 批次，必须重新用同一套 `(1,243)` 测。

---

## 5. 按键与状态机

- 原厂 `KeyInit`（0x4201DE5C）：GPIO **2/4/6/7/8** 配成输入；反汇编里 `pull_up_en=0`，
  **原厂没有开内部上拉**。badapple 开了内部上拉，只是让悬空脚有确定电平。
- **2026-11-25 真机映射**：GPIO7=MENU、GPIO8=OK 已实测；GPIO6 暂按 FUNC（原 MENU 位），
  GPIO2/4 仍是 PREV/NEXT 推断。不要再用“6=MENU/7=OK/8=FUNC”的旧假设。
- 扫描任务：20 ms 轮询，连续 3 次低电平算按下，3 次高电平算松开；只上报**按下沿**。
- 按键任务只置一个 `volatile bool` 事件，回放任务消费；全刷期间按下的键不会丢。
- 状态机：
  - 待机：广播开，显示 Hello Paperang + `BLE:x`；
  - 按键：广播关，2 fps 播放；
  - 再按：停止、全刷清屏回待机、广播开。
- 广播开关用 `s_adv_enabled` 标志 + host 任务事件，不要在回放任务里直接调 GAP。

---

## 6. 构建与配置

关键 sdkconfig（与第三方固件相关）：

```
CONFIG_IDF_TARGET="esp32s3"
CONFIG_ESPTOOLPY_FLASHSIZE_8MB=y
CONFIG_ESPTOOLPY_FLASHMODE_DIO=y
CONFIG_ESPTOOLPY_FLASHFREQ_80M=y
CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ_240=y
CONFIG_FREERTOS_HZ=1000
CONFIG_BT_ENABLED=y / CONFIG_BT_NIMBLE_ENABLED=y
CONFIG_BT_NIMBLE_ATT_PREFERRED_MTU=247
CONFIG_BT_NIMBLE_HOST_TASK_STACK_SIZE=6144
CONFIG_BT_NIMBLE_MEM_ALLOC_MODE_INTERNAL=y
CONFIG_ESP_PHY_CALIBRATION_AND_DATA_STORAGE=n   # ← 本文 §3 的重点
CONFIG_SPIRAM=n
CONFIG_ESP_WIFI_NVS_ENABLED=n
CONFIG_ESP_TASK_WDT_TIMEOUT_S=5
```

- 构建产物必须是 **app-only** 镜像；用 `esptool image_info` 检查 checksum / appended hash / segments。
- 本地分区表只用于编译（app 必须小于所选分区），不要把本地表/bootloader 写进设备。
- 当前 app：472,704 B，6 段，DROM 0x143B4、IROM 0x44F18、IRAM 合计 0x15A28。
- 不要依赖 `CONFIG_PARTITION_TABLE_*` 的编译期值做运行时判断；一切以 `esp_partition_find*` 读到的 flash 表为准。

---

## 7. 调试方法（这次真正救命的）

1. **面板当日志**：待机画面显示 `BLE:1..6`；diag 里带 `fr=ok,c=1314,fps=6`、`route`、`key=掩码`。
2. **控制变量**：
   - 原厂 vs 自定义（判断是 app 还是平台）；
   - 冷启动 vs 原厂启动后软重启（判断 PHY/射频状态是否继承）；
   - 待机（只广播）vs 播放（面板刷新）——先排除“回放干扰 BLE”的假设。
3. **独立验证**：两个扫描器 + 按 MAC 直连，不能只看一个 API 的返回码。
4. **宿主侧预渲染**：把 `idle_screen()` 用 gcc + font.c 编译成 PGM/PNG，先看布局再刷机，省一次写入。
5. **一次只改一个变量**：每次写入成本高，先把假设写成可观察的状态码。
6. **保留已知可用镜像 + 原厂整片 dump**，每次写入后回读校验。

---

## 8. 可复用资产

| 路径 | 用途 |
|---|---|
| `dump/e2s_flash_8mb_r5.bin` | 原厂 8 MB 整片 dump（恢复基线） |
| `dump/FLASH_DUMP_FINDINGS.md` | dump 解析结论（分区/字体/NVS/身份） |
| `tools/e2s_ble_flash.py` | scan/info/restore/flash/dump/full；`--force` 跳过低电量门限 |
| `tools/e2s_flash_check.py` | 分区表/otadata/app 描述符校验 |
| `firmware/hello_paperang` | 最小 BLE + EPD + A5 示例；8×16 字体 |
| `firmware/badapple` | 待机/按键/2 fps/外置帧区完整实现 |
| `firmware/e2s_re` | 原厂 1.3.739 逆向还原（KeyInit 引脚、A5、EPD、LUT） |

帧区布局：app `0x20000`、otadata `0xF000`、帧区 `0x530000`；镜像 8 MB，区域写入时镜像尺寸必须等于整片。

---

## 9. 后续值得做的软件工作

1. **把 §4.5 剩下的标注做完**：全刷/局刷、BOE BUSY 极性、五个按键脚已验证；还差“只按指定丝印键”的单键映射、GPIO5 插拔对照、GPIO34 的 SD 挂载实验（需 SD 机）、OED 实机。
2. **安全的出厂校准加载**：只读解析 NVS `phy`，或把校准 blob 嵌进 app；目标：冷启动不依赖“先启动原厂”。
3. **修等待逻辑**：BOE 全刷等 BUSY 低电平（上限 5 s），局刷等“拉高后再回低”（上限 2 s），
   完成后再写 `0x26`。这能把开机/停止的 28 s 降到 ~1.5 s，并消除残影上游。
4. **帧率就定 2 fps**：局刷实测 454 ms，3 fps（333 ms）必撕裂；除非换更快的面板/波形。
5. **自定义固件内实现 OTA**（带同样的“写非运行槽 + 校验 + 回退”约束），或单独的帧区更新通道。
6. 深睡/唤醒、音频、SD、原厂 UI 组件——本次未涉及。
7. 始终遵守：不写 bootloader/分区表/NVS；原厂 ota_1 不动；任何持久写入前先确认。
