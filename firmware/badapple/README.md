# Bad Apple!! on E2S

把完整版 Bad Apple!! 塞进 E2S 单词卡：438 帧、250×122、2 fps，循环播放。

这是一个 **App-only** 固件，只通过原厂 OTA 服务写入非运行槽。原厂 1.3.739 槽不动，
固件里的 A5 服务仍然可以把启动槽切回原厂。

## 运行方式

- 开机先进待机：全刷一次 “Hello Paperang + BLE:x”，保持 BLE 广播；
- 按任意键：关广播，2 fps 播放（1314 帧里每 3 帧取 1 帧，共 438 帧 / 219 秒），
  播完停 3 秒后从头循环；
- 播放中再按键：停止、全刷回待机画面、重新开广播。
- 帧数据不编进 app：RLE 后 220,710 B 放在 flash 空白区 `0x530000`
  （原厂字体区之后），启动时只读 16 B 头 + 帧表 + 单帧 2 KB。
- 第 0 帧、每 96 帧、以及画面变化超过 90% 的帧走全刷（共 6 次），用于清残影并重同步
  SSD1680 的 old RAM；其余帧走局部刷新：
  `0x24` 写当前帧 → `0x22 0xFC` → `0x20`，完成后把同一帧写回 `0x26`。
- 无声。E2S 有喇叭和 OPUS 解码，但这一版只做屏幕动画。

## 编译

需要 ESP-IDF 5.4：

```sh
. $IDF_PATH/export.sh
idf.py -C firmware/badapple -B firmware/badapple/build build
```

产物：`firmware/badapple/build/badapple_e2s.bin`（App-only，475,040 B < 0x1F0000）。

**不要用 `idf.py flash`，不要写 bootloader/分区表。**

## 刷入

设备需要处于原厂固件并打开单词卡同步模式（BLE 正在广播）。

- App：`刷入固件` → `选择文件` → `badapple_e2s.bin`；
- 命令行（Linux + BlueZ）：

```sh
python3 tools/e2s_ble_flash.py flash firmware/badapple/build/badapple_e2s.bin
```

命令行工具会先做 A5 握手、型号/电量/参数检查，再复用原厂 OTA 服务上传镜像并触发启动。
如果当前不是原厂固件，它会先调用 `(1,240)/(1,19)` 切回原厂槽；原厂重启后若没有自动广播，
按一下单词卡按键唤醒。

## 恢复原厂

播放期间 A5 服务仍在广播。App 的 `恢复原厂`，或：

```sh
python3 tools/e2s_ble_flash.py restore
```

恢复只改 otadata 的启动槽选择，不会覆盖原厂镜像。

## 诊断

`(1,241)` 返回只读诊断，动画相关的字段：

- `an=` 已完成的局部刷新次数
- `pm=最近一次耗时/平均耗时`（ms）
- `fm=` 全刷次数，`fms=` 最近一次全刷耗时
- `kc=` GPIO 2/4/6/7/8 各自的按下次数，`last=脚/按住毫秒`：逐个按键即可建立
  “丝印键 → GPIO”映射
- `g34= / g5=` GPIO34/GPIO5 的实时原始电平（只读，不驱动）
- `tm=` 最近一次 `(1,243)` 严格计时结果

### EPD 真实时序测量

`pm/fms` 是保守等待的返回值，不是面板完成时间。要测真实值：

1. 发 `(1,243)`：后台执行一次严格全刷 + 严格局刷（每步等 BUSY 真正回到低电平）；
2. 严格路径实际只要约 2 s（全刷 1.31 s + 局刷 0.45 s），发 `(1,244)` 读结果。

字段：

- `full=` 从 0x12 到 0x20 后空闲的总毫秒数；`ph=a/b/c/d` 是 0x12 后、配置后、
  写帧后、0x20 后四个阶段各自到空闲的毫秒数
- `bm=` 四个阶段是否见过 BUSY=1 的位图；`ft=` 哪些阶段超时（正常为 0）
- `ev=+毫秒:电平,...` 是 0x20 后 BUSY 的每次跳变（用来确认极性）
- `part=` / `pt=` / `pb=` / `ev=` 是局刷的同一组数据
- `tm=-1` 表示面板不是 BOE（本测量只实现 BOE），`tm=run` 表示还在跑

BLE 状态：

- `ble: synced= conn= adv= need= rc= c= d= ac= rst=`
- `conn` 当前连接号，`adv` NimBLE 是否认为广播在跑，`need` 是固件的“必须广播”标志
- `rc` 最近一次 `ble_gap_adv_start` 的返回码（0 成功；6=ENOMEM，15=EBUSY，2=EALREADY）
- `c/d/ac` 连接/断开/ADV_COMPLETE 计数，`rst` 是看门狗恢复次数

正常局刷的 `pm` 在 ~126–470 ms 之间；如果接近 2000 ms，说明局部刷新没有生效（面板仍用全刷 LUT），
需要换刷新策略。`pm` 只是驱动等待的返回值，真实完成时间用 `(1,243)/(1,244)` 测（实测 454 ms）。

## BLE 广播看门狗

旧版在 `BLE_GAP_EVENT_DISCONNECT` 里只调用一次 `ble_gap_adv_start()`。链路拆除边缘的
HCI `LE Set Adv Enable` 可能返回 `EBUSY`/`ENOMEM`，失败后没有重试，设备会一直静默
（屏幕还在播，但扫不到）。手机 App 刷机后的 `autoConnect` 校验连接/断开正好会踩到这个
边缘。

现在的处理：

- 断开时先 `ble_gap_adv_stop()` 清掉可能残留的 host 广播状态，再投递广播事件；
- 回放任务每 5 秒调用 `ble_service_tick()`：发现连接号失效就清掉重播；`s_need_adv`
  为真时停一下再 start，失败会持续重试；
- `(1,241)` 的 `rc/rst` 可以直接确认是否还在发生广播启动失败。

## 整片备份（不需要编程器/EN）

E2S 的 flash 和 ESP32-S3 共用 SPI。CH341A 夹片读取时 ESP32 已上电并在读同一片 flash，
总线争用会让 dump 随机失败（两遍 SHA 不同、大片 `0xFF`）。找不到 EN 测试点时不用强求：
让固件自己当 SPI 主机读。

固件提供只读命令（握手后可用）：

- `(3,2)` → flash 大小 u32 + JEDEC ID u32
- `(3,1)` content=`offset u32 LE + length u16 LE` → 原始字节（≤512 B）

桌面工具每 256 KiB 打印进度，断链后自动重连并从断点续读：

```sh
python3 tools/e2s_ble_flash.py dump dump/e2s_flash_8mb_r3.bin
python3 tools/e2s_ble_flash.py dump dump/e2s_flash_8mb_r4.bin
sha256sum dump/e2s_flash_8mb_r3.bin dump/e2s_flash_8mb_r4.bin
python3 tools/e2s_flash_check.py dump/e2s_flash_8mb_r3.bin
```

`e2s_flash_check.py` 会解析引导头、分区表、两个 OTA 槽的 app 描述符和 otadata，
直接打印当前启动槽；它只读文件，不会写设备。

有效判据：8,388,608 B、`0x0=0xE9`、`0x8000` 附近有 `AA 50` 分区表、两遍 SHA 相同。
读取期间动画继续播放是正常的。8 MiB 走 BLE 大约几分钟。

## 重新生成帧

源视频 480×360、30 fps。先按 6 fps 抽成 162×122 灰度原始帧：

```sh
ffmpeg -i badapple.mp4 -vf "fps=6,scale=162:122,format=gray" \
       -f rawvideo -pix_fmt gray frames_wide.raw
```

再生成 `frames.bin`（脚本会逐帧回读校验）：

```sh
python3 tools/build_frames.py frames_wide.raw main/frames.bin
idf.py -C firmware/badapple -B build/badapple app
```

布局：250×122，每行 32 B，MSB 在前，`1=白`；左右各 44 px 黑边（162 px 宽的画面居中）。
每帧是 LEB128 游程，0/1 交替、从 0 开始。文件头 16 B（`BAPL`、版本、fps、宽高、帧数），
随后每帧 8 B 表项（payload 偏移 + full 标志）。

想改全刷频率：改 `tools/build_frames.py` 里的 `k % 96` 和 `ratio > 0.90`，重新生成即可。

## 风险

- 局部刷新使用 SSD1680 的 OTP 波形；不同批次面板的残影/刷新时间可能不同。全刷频率
  可以在帧数据里调整。
- 自定义固件如果起不来，只能 UART/esptool 恢复。原厂槽仍在 flash 里，但需要有拆机或
  UART 接触条件才能救回。
