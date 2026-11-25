# E2S 阅读器固件资产盘点

日期:2026-11-24。范围:`firmware/e2s_re`、`firmware/hello_paperang`、`firmware/badapple`,以及 `dump/`、`docs/`、`tools/`。
方法:读源码 + 读文档 + 对原厂 1.3.739 反汇编段(`tools/fw_ext/seg*.bin`)用 xtensa objdump 定点复核 + 对 8 MiB 整片 dump 做十六进制核对。
**本次没有运行 `idf.py build`、没有刷机、没有做任何真机测试**;凡标"实测"的数字都来自仓库文档的真机记录,不是本次测量。

---

## 0. 一句话结论

- **可直接用的主线资产在 `badapple`(最完整)和 `hello_paperang`(最小)**:EPD 双面板族驱动(含 BOE 局刷)、按键、NimBLE A5 恢复服务、LVGL 8.2.0 vendor、8×16 ASCII 字模、8 MiB flash 运行时布局。
- **`e2s_re` 的价值是"协议/常量/逆向地址",不是驱动代码**:它的 `epd.c` 引脚、SPI host、BUSY 等待、LUT 切片都有硬错误;`pkey`/`battery_check` 是空骨架;`partitions_re.csv` 基于错误的 4 MB 假设;A5 的 CRC 初值也错。
- 关键数字:**8 MiB flash**(不是 4 MB)、面板 **250×122 可视 / 250×128 画布**、全刷 **1,312 ms**、局刷 **454 ms**、**冷启动首帧必须全刷**、OTA 槽 **0x1F0000**。
  (2026-11-24 真机严格计时更正:旧值 28,007 ms / 126 ms 是等待策略的返回值,不是面板完成时间,见 §4.3。)

---

## 1. 硬件与构建事实(权威值)

| 事实 | 值 | 证据 |
|---|---|---|
| SoC | ESP32-S3,8 段 app 镜像,magic 0xE9 | `dump/e2s_flash_8mb_r5.bin` @0x210000;`docs/FLASH_DUMP_FINDINGS.md:48-57` |
| 内部 flash | **8 MiB**,GD25Q64,JEDEC `C8 40 17` | `docs/E2S_THIRD_PARTY_FIRMWARE.md:26`;dump 文件 8,388,608 B;`dump/FLASH_DUMP_FINDINGS.md:22-23` |
| flash 模式 | DIO,80 MHz,容量码 8 MiB | `docs/E2S_THIRD_PARTY_FIRMWARE.md:27`;`badapple/sdkconfig:530-535`;`dump/FLASH_DUMP_FINDINGS.md:44` |
| 运行时分区表 | nvs 0x9000/0x6000;otadata 0xF000/0x2000;phy_init 0x11000/0x1000;ota_0 0x20000/0x1F0000;ota_1 0x210000/0x1F0000 | 本次 `od -A x -t x1z -j 0x8000 -N 160 dump/e2s_flash_8mb_r5.bin` 逐字节读出(见 §2) |
| otadata 现状 | entry0 seq=43,state=0xFFFFFFFF,CRC 0x88CCD7C0 → ota_0 | 本次 `od ... -j 0xf000 -N 64`;`dump/FLASH_DUMP_FINDINGS.md:62-72` |
| PSRAM | 原厂代码确实用 `MALLOC_CAP_SPIRAM`(LVGL 32 KB 池、FreeType 堆、EPD 帧缓冲 4000 B);容量 2 MB **仅来自设备自报 JSON,未实测** | `e2s_re/docs/VERSIONS.md:69,87-88`;`e2s_re/components/ble/paperang_ble.c:97-101`;本次反汇编 0x4202a9d7-0x4202a9e0 |
| 面板 | 2.13" 墨水屏,250×122 可视,1bpp,**面板 RAM = 250 行 × 16 B = 4000 B**(250×128 位,122 行有效,每行 6 bit 填充) | `hello_paperang/README.md:18-25`;`badapple/main/epd.h:12-13`;`badapple/main/epd.c:193-210`;原厂 `heap_caps_malloc(0xfa0=4000, 0x400)` @0x4202a9d7 |
| LVGL 画布 | 250×128(full_refresh 位被置 1,但 pristine v8.2 会因 8000 px 缓冲清 0 → 实际 32 行/band) | `e2s_re/docs/VERSIONS.md:20,96`;`e2s_re/components/pgui/pgui_api.c:20-36` |
| EPD 引脚 | **CS=40,DC=39,RST=48,BUSY=47,MOSI=41,SCLK=42**;无 PWR 脚,GPIO46 未使用 | 本次反汇编 0x4202a828-0x4202a8e8(47 输入;48/39/40 输出上拉)、0x420298f4(pre_cb 置 39)、0x42029a14/0x42029a6c(40 拉低/拉高);`hello_paperang/main/epd.h:5-10`;`badapple/main/epd.h:5-10` |
| EPD SPI | **SPI3_HOST(host id 2)**,3-wire + half-duplex,mode 0,10 MHz,CS 手动(`spics_io_num=-1`),DMA auto,max_transfer 4000 | 本次反汇编 0x4202a93c-0x4202a97b(`movi a10,2`;flags 0x14;10 MHz;queue 16;pre_cb 0x420298f4);`hello_paperang/main/epd.c:351-365` |
| EPD 复位 | RST 低 11 tick → 高 11 tick(FREERTOS_HZ=1000 时约 11 ms) | 本次反汇编 0x420298c8-0x420298ed;`hello_paperang/main/epd.c:138-145` |
| 面板分族 | 复位后读 GPIO47:0→BOE(type 1,不读 ID);1→读 0x70 5 字节,id[2]==5→OED(type 3),否则 GD(0) | 本次反汇编 0x4202aabf-0x4202aae4;`hello_paperang/main/epd.c:9-11,371-385` |
| 按键 | **GPIO 2/4/6/7/8,输入,低电平按下**;原厂 `gpio_config` 里 pull_up_en=0(未见内部上拉),500 ms 长按定时器 | 本次反汇编 0x4201de5c-0x4201df31(掩码 4/0x40/0x100/0x80/0x10 → 2/6/8/7/4,逐个 `gpio_set_direction(...,1)`);`badapple/main/key.c:1-12` |
| 电量 ADC | **ADC1_CHANNEL_0(SoC 固定映射到 GPIO1)**,12-bit,11 dB atten,`esp_adc_cal`;电压 = 引脚 mV × 2;百分比 = clamp((Vbat+25−3500)×100/600, 0, 100) | 本次反汇编 0x4202dbf8-0x4202dd18(0x4202dc21 channel=0;0x4202dc7c `slli 1`;0x4202dce6-0x4202dd15 门限 4099/3500) |
| 电量分压使能 | **GPIO45**:采样前置 1、采样后置 0(每次采样都切) | 本次反汇编 0x4202db28-0x4202db50(仅 `gpio_set_level(45,arg)` + 10 tick)与 0x4202dc02/0x4202dce1 调用 |
| 充电/电源输入 | GPIO5 配成输入并读取,`level==0` 作布尔返回(疑似充电检测,语义未确认) | 本次反汇编 0x4202dbd0-0x4202dbf5;调用者见 `tools/fw_ext/func_callers.json`(0x420299b0/0x4202ac1c/0x4202e158 等) |
| SD | **SDMMC 4-bit**:CLK=11,CMD=12,D0=13,D1=14,D2=9,D3=10,内部上拉,slot 1;挂载 `/sdcard`,FATFS;另有 **SD 电源/复位控制脚 GPIO34(挂载期间保持低,置低后延时 50 ms 再挂载;卸载后置高)** | 本次反汇编 0x42009bf4-0x42009c20(槽位字段直写 11/12/13/14/9/10,width=4,flags=1)、0x42009b34-0x42009b76(置低+50 tick,保持低)、0x42009ce5(卸载置高) |
| SD 挂载重试 | 3 次,间隔 500 ms;失败只打印 `FATAL: SD card error` **然后继续启动**,不 return | 本次反汇编 0x420099a5-0x420099d7、0x42009ac4-0x42009acd |
| 字体区(分区表外) | 0x480000–0x528884,含 3 个 FTF(`0x40` B 头 + TTF);0x520000–0x528884 是 1bpp 位图缓存 | `dump/FLASH_DUMP_FINDINGS.md:88-123`;本次 od 逐段核对 |
| 空白区 | 0x12000–0x20000(57,344 B)、0x400000–0x480000(512 KiB)、0x528884 以后到 0x800000(约 2.84 MiB)全 0xFF | `dump/FLASH_DUMP_FINDINGS.md:46,91`;`badapple/main/playback.c:45-46` |
| CPU 频率 | badapple/hello 240 MHz;e2s_re 生成的 sdkconfig 是 160 MHz | `badapple/sdkconfig:1494`;`e2s_re/sdkconfig:2656`;`docs/E2S_THIRD_PARTY_FIRMWARE.md:212` |
| 看门狗 | `CONFIG_ESP_TASK_WDT_TIMEOUT_S=5` | `badapple/sdkconfig:1576`;`docs/E2S_THIRD_PARTY_FIRMWARE.md:221` |

---

## 2. flash 与分区:4 MB 还是 8 MB?

### 2.1 冲突证据

| 主张 | 出处 |
|---|---|
| 4 MB | `e2s_re/sdkconfig.defaults:12-13`(注释 `device info JSON "RomSize":"4M"`)、`e2s_re/partitions_re.csv:2`、`hello_paperang/README.md:104`("内部 4 MB flash") |
| 8 MB | 两个 8,388,608 B 的整片 dump(`dump/e2s_flash_8mb_r5.bin`/`r6.bin`,SHA 相同);`dump/FLASH_DUMP_FINDINGS.md:22-23`;`docs/E2S_THIRD_PARTY_FIRMWARE.md:26`(GD25Q64,JEDEC C8 40 17);`badapple/README.md:109`;`badapple/sdkconfig:539`、`hello_paperang/sdkconfig.defaults:2` |

### 2.2 权威来源与结论

**结论:8 MiB。** 4 MB 的唯一来源是原厂固件**自报**的设备信息 JSON 字段 `"RomSize":"4M"`(`e2s_re/components/ble/paperang_ble.c:97-101`,逐字节还原自 DROM 0x3C16C7BE),它描述的是"设备信息字符串",不是 flash 芯片容量。`hello_paperang/README.md:104` 沿用了这个错误。e2s_re 的 `partitions_re.csv` 正是在此错误上做的"恰好容纳 2,026,352 B 官方镜像"的推断(`partitions_re.csv:1-7`),`e2s_re/README.md:105` 自己也注明"分区表未实测"。

### 2.3 运行时分区表(本次从 dump 0x8000 逐字节读出)

```
0x8000  aa50 0102 00900000 00600000 "nvs"
0x8020  aa50 0100 00f00000 00200000 "otadata"
0x8040  aa50 0101 00100000 00100000 "phy_init"
0x8060  aa50 0010 00000200 0000 1f00 "ota_0"   (offset 0x20000, size 0x1F0000)
0x8080  aa50 0011 00002100 0000 1f00 "ota_1"   (offset 0x210000, size 0x1F0000)
```

- `ota_0` 与 `ota_1` 各 2,031,616 B;`ota_1` 结束于 0x400000。0x80A0 起是分区表 MD5 条目(`EB EB` + 16 B MD5,本次 od 已核对)。
- 与 `partitions_re.csv` 的差异:nvs 0x5000 vs **0x6000**;otadata 0xE000 vs **0xF000**;phy_init **0x11000**(重建表没有);ota_0 0x10000 vs **0x20000**;槽 0x1EF000 vs **0x1F0000**。
- 0x12000–0x20000(0xE000)未分配;0x400000 以后无分区条目,但 0x480000 起有字体数据。
- 原厂 app 身份:project `paperang_e2`、version `c6b2d574b-dirty`、app desc magic 0xABCD5432(本次 od @0x210020)。

### 2.4 对新阅读器固件的直接约束

1. 只能刷 **app-only** 镜像到非运行槽;bootloader/分区表/NVS/otadata/ota_1 不动(`docs/E2S_THIRD_PARTY_FIRMWARE.md:11-12,38-40`;`hello_paperang/README.md:52`;`badapple/README.md:28`)。注意运行时分区表来自 flash,编译期用 `SINGLE_APP_LARGE` 只是为了让本地构建有足够大的 app 分区(`badapple/sdkconfig.defaults:14-22`)。
2. app 必须 < 0x1F0000(2,031,616 B)。参考:badapple 472,704 B,hello 485,664 B,e2s_re v6.1.1 构建 1,482,272 B。
3. 书/缓存/字库可放 0x530000 起的裸 flash(badapple 的 frames.bin 就是这么放的,`badapple/main/playback.c:45-46`),或 0x400000–0x480000;不要碰 0x480000–0x528884 的原厂字库。OTA 只写 ota_0/ota_1,不会碰这些裸区。
4. e2s_re 的 4 MB `sdkconfig`/`partitions_re.csv` **不能**用于整片镜像或分区表生成;只做 app-only 构建时不影响,但会误导。

---

## 3. 冲突核对(逐条给权威结论)

### 3.1 EPD 引脚与 SPI host

- e2s_re:`CS=39, DC=46, RST=48, PWR=40, BUSY=47, MOSI=41, SCLK=42`,`SPI2_HOST`,`epd.h:10-25`、`epd.c:8,40-47`。
- hello/badapple:`CS=40, DC=39, RST=48, BUSY=47, MOSI=41, SCLK=42`,`SPI3_HOST`(`epd.h:5-10`,`epd.c:5-7`)。
- 反汇编(权威):
  - 0x4202a828:GPIO47 设输入;GPIO48、39、40 设输出并上拉 → 输出只有 **48/39/40**,没有 46。
  - 0x420298f4(pre_cb):`if (t->user != 2) gpio_set_level(39, t->user)` → **DC=39**。
  - 0x42029a14(cmd)/0x42029a6c(data):发送前 `gpio_set_level(40,0)`,发送后 `1` → **CS=40**。
  - 0x4202a945:`movi a10,2` 传给 `spi_bus_initialize` → host id 2 = **SPI3_HOST**(IDF 中 SPI2_HOST=1,SPI3_HOST=2)。
- **结论:hello/badapple 正确,e2s_re 的 CS/DC/PWR 三角色全部错位,GPIO46 是凭空多出来的。** e2s_re 自己的注释 `epd.c:8` 其实记下了 `gpio_set_level(48,1) (39,1) (40,1)`,却把 39/40 标成了 CS/PWR。
- 风险:GPIO46 在 ESP32-S3 是启动 strapping 脚,按 e2s_re 把它当 DC 输出有把设备带进下载模式的风险。

### 3.2 LUT 波形

- e2s_re `epd_luts.h`:15 张表,含 `lut_pair_v60[55] @0x3C17B638`、`lut_pair_v90[55] @0x3C17B5C8`、`pat_old_new_a[16] @0x3C17B6E0`、`pat_old_new_b[16] @0x3C17B6F8` …
- hello/badapple `epd_lut_stock.h`:`s_lut20[55] @0x3c17b6a9`、`s_lut24[55] @0x3c17b5c9`、`s_lut22[55] @0x3c17b639`、`s_lut23[55] @0x3c17b601`、**`s_lut153[153] @0x3c17b6e0`**。
- 本次逐字节比对:hello 的 4 张 55 B 表 == e2s_re 对应表的 **`[1:]`(少一个前导 0x01)**,第 55 字节补 0(来自 DROM 下一字节)。也就是说 e2s_re 把每张 UC8151 LUT 的起点统一提前了 1 字节。
- 更严重:0x3c17b6e0 是 **153 B 的 BOE LUT**(`s_lut153`,首 16 B 恰好是 `AA 80 55 40 00…54 10 A8 20`),e2s_re 把它切成了两个 16 B 的 `pat_old_new_*`,并继续把后面的 booster/vcom 表按 10/7 B 切分。e2s_re **完全没有 153 B 的 BOE 全刷 LUT**,因此它的 BOE 路径不可能正确。
- **结论:直接复制 `badapple/main/epd_lut_stock.h`(与 hello 完全相同,`diff -q` 一致)。e2s_re 的 LUT 只能当 DROM 地址索引,不能喂给驱动。**

### 3.3 A5 CRC 初值

- e2s_re:`esp_rom_crc32_le(0, …)`,注释称 "zlib standard"(`paperang_ble.c:34,154,634`)。
- hello/badapple:`crc32_calc()` 用初值 **896963873(0x35769521)** 的反射 CRC-32,无末尾异或(`badapple/main/a5_server.c:94-113`);`hello_paperang/README.md:100` 明确"不是 zlib 默认值",且 hello 的真机范围与官方 App 流程已跑通(`hello_paperang/README.md:102-106`)。
- 独立 oracle:`tools/A5ProtocolTest.java:57-64` 用同一 OEM 初值实现 `oemCrc()`,并注明"written independently of the production helper";它跑的是 hello 的 C A5 服务器(`A5ProtocolTest.java:18`),不是 e2s_re。反例:`tools/mock_e2s.py:59,149` 用的是 `zlib.crc32`(初值 0xFFFFFFFF),与 OEM 不同,所以"对 mock 差分通过"不能证明与官方 App 兼容。
- **结论:以 hello/badapple 为准。** e2s_re 的 A5 实现与官方 App 的帧 CRC 不兼容,不能直接复用;若要用 e2s_re 的命令语义,必须把 CRC 换成 896963873。

### 3.4 SD 引脚

- e2s_re `main/sdcard.c:22-38`:`CLK=14, CMD=15, D0=2, width=1`,并明确写 "TODO: recover real slot pins"。
- 反汇编(权威):host 用 `SDMMC_HOST_DEFAULT()`(slot 1),slot 用 `SDMMC_SLOT_CONFIG_DEFAULT()` 后覆盖 `clk=11, cmd=12, d0=13, d1=14, d2=9, d3=10`,并置 `width=4`、`flags=1`(内部上拉)(0x42009bf4-0x42009c20;DROM 默认结构 0x3c164150 与之对应,IDF v4.4.4 `sdmmc_slot_config_t` 字段序 clk/cmd/d0..d7/cd/wp/width/flags)。
- GPIO34 的完整时序(2026-11-25 复核 `tools/fw_ext/seg3_0x42000020.bin`):
  0x42009b34 是带一个布尔参数的辅助函数 —— `a2!=0`(挂载)时
  `gpio_set_level(34,0)` → 延时 50 tick → 返回,**之后一直保持低**;
  `a2==0`(卸载)时只 `gpio_set_level(34,1)`。挂载重试(0x420099c6)每次用
  `a2=剩余次数` 重走“置低 + 50 ms”,卸载路径(0x42009ce5)用 `a2=0` 置高。
  **即 GPIO34 低 = 卡使能,高 = 断电/复位。**
- **结论:使用 11/12/13/14/9/10 + width 4;GPIO34 挂载期间保持低,卸载后置高。** e2s_re 的 GPIO2 还与按键 GPIO2 直接冲突,证明那组是占位猜测。

### 3.5 显示高度 122 vs 128

两者都对,但指的不是同一个东西:
- **可视区 122 行**(`badapple/main/epd.h:13`、`docs/E2S_THIRD_PARTY_FIRMWARE.md:145`)。
- **LVGL 画布/面板 RAM 128 行**:原厂 `ver_res=128`(`VERSIONS.md:20,96`),帧缓冲 `4000 B = 250×128/8`(原厂 `heap_caps_malloc(0xfa0)` @0x4202a9d7)。128 行里只有 122 行可见,每行尾部 6 bit 是填充。
- 逻辑图缓冲是 **250 宽 × 122 高,行宽 32 B(3904 B)**;面板 RAM 是 **250 行 × 16 B(4000 B)**,由 `build_fb()` 旋转 + y 翻转(`badapple/main/epd.c:193-210`)。`hello_paperang/README.md:25` 说"行宽 16 字节"容易被误读成逻辑缓冲,实际那是面板 RAM。

### 3.6 启动失败处理

- e2s_re `main/main.c:95-99`:SD 挂载失败 → 打印 FATAL → **return**。
- 反汇编(权威):0x420099a5 挂载循环;3 次失败后 0x42009ac4 打印 `========> FATAL: SD card error <========`,然后 `j 0x420099dc` **继续** word_manager/player/EPD/Key/BLE(`main.c` 的启动序列在 0x420099dc 之后)。
- **结论:原厂不因 SD 失败中止。** 阅读器更应如此:SD 缺失也要能显示"无卡"界面并通过 BLE 恢复。

### 3.7 按键内部上拉

- `badapple/main/key.c:1-5` 注释称原厂 `KeyInit` 逐个 `gpio_pullup_en()`。
- 反汇编 0x4201de99-0x4201df31:`gpio_config` 结构里 `mode=1`、`pull_up_en = a4`(a4 = `esp_timer_create` 的返回值 0)、`pull_down_en=0`、`intr_type=0`;随后对 5 个脚 `gpio_set_direction(pin, 1)`。即 **代码没有开内部上拉**。
- 实践上 badapple 打开内部上拉没有坏处(若板上有外部上拉则并联),但"原厂开了上拉"这个说法不准确。

---

## 4. 组件盘点

### 4.1 EPD 驱动

#### A. 可用实现:`badapple/main/epd.c`(+`hello_paperang/main/epd.c`,同一份 r22)

API(`badapple/main/epd.h:16-23`):

```c
int  epd_init(void);
int  epd_draw(const uint8_t *img);                  /* 强制全刷 */
int  epd_draw_partial(const uint8_t *img);          /* BOE 局刷;其他族回退全刷 */
int  epd_draw_ex(const uint8_t *img, int full);     /* full=1 全刷,0 尝试局刷 */
void epd_deep_sleep(void);                          /* 当前实现是空操作 */
void epd_diag(char *out, int n);                    /* 只读诊断串 */
```

- 初始化:`epd_init()` 配 GPIO(40/39/48 输出,47 输入上拉)、`spi_bus_initialize(SPI3_HOST, …, SPI_DMA_CH_AUTO)`、10 MHz/3-wire/half-duplex/pre_cb,然后复位 + 100 ms,读 GPIO47 分族;47==1 时读 0x70 5 字节但**一律按 OED 处理**(`epd.c:326-391`)。
- BOE 全刷:`0x12 → 0x01 FA0000 → 0x11 03 → 0x21 0080 → 0x18 80 → 0x3C 05 → 0x3F 22 → 0x03 17 → 0x04 41A832 → 0x2C 58 → 0x32 [153B LUT] → 窗口 → 0x24/0x26 各写 4000 B → 0x22 C7 → 0x20`(`epd.c:289-324`)。注意 BOE 帧走 **0x24/0x26**,不是 0x13。
- OED 全刷:UC8151 风格初始化 + 4 张 55 B LUT(0x20/0x24/0x22/0x23)+ **0x10 与 0x13 都写本帧**(避免未初始化 old bank 导致全黑)+ `0x17 A5` + `0x41 07`(`epd.c:233-286`)。
- 局刷(仅 BOE):`窗口 → 0x24 当前帧 → 0x22 FC → 0x20 → wait_busy_cycle(120,6000) → 窗口 → 0x26 回写同一帧`(`epd.c:354-374`)。**0x26 回写是必须的**,否则多帧后残影/该刷不刷。
- BUSY 策略:不用固定极性,`wait_reaction()` 等电平翻转再回原电平(`epd.c:114-138`);局刷 `wait_busy_cycle()` 起始只等 120 ms(`epd.c:141-164`)。全部等待都有超时,超时后仍继续(不返回错误)。
- 极性:`1=白,0=黑`;输入图 `row_bytes=(250+7)/8=32`,MSB first(`epd.h:17`,`epd.c:393`)。

**复用方式:直接复制**(连同 `epd_lut_stock.h`)。注意:OED 分支只做全刷(`epd.c:451` `did_full = full || s_family != 1`),若目标机是 OED,翻页会走全刷(BOE 实测 1.31 s;OED 未测),必须补 OED 局刷或确认机型。

#### B. e2s_re 的 `components/epd`(不可直接复用)

API(`components/epd/epd.h:12-28`):

```c
void epd_init(void);
void epd_refresh(int mode);              /* 0 full, 1 partial, 2 buf, 3 sleep, 4 wake */
uint8_t *epd_get_framebuffer(void);      /* 4000 B,idx=y*250+x,MSB first */
int epd_get_panel_type(void);            /* 1=BOE,3=OED,0=GD */
```

问题清单(每条都有反证):

1. **引脚错**:`epd.c:41-47` CS=39/DC=46/PWR=40;权威是 CS=40/DC=39,且无 46/无 PWR(§3.1)。
2. **SPI host 错**:`epd.c:40` `SPI2_HOST`;原厂 host id 2 = SPI3_HOST。
3. **BUSY 等待错**:`epd.c:84-89` `while (n-- > 0 && gpio_get_level(BUSY) == 0) vTaskDelay(10ms)`,即"BUSY 为低才等";原厂 0x420298a3 是 **5 tick 轮询 + 5000 次上限 + BUSY==0 即返回**(约 25 s),极性/计数单位都不同。按 e2s_re 的写法,`epd_refresh(0)` 传 5000/20000 会变成最长 **50 s / 200 s** 阻塞。
4. **分族缺失**:`epd_init` 无条件读 0x70,没有"复位后读 GPIO47"这一步(原厂 0x4202aabf-0x4202aae4)。
5. **刷新是异步的**:原厂 `epd_refresh` 只置全局 mode 并唤醒 epd 任务(0x42029d9c),真正的命令序列由 0x4202982c 任务按 0x3c17b488 模式表执行;e2s_re 写成了同步直调。行为可用但与原厂不同。
6. **LUT 切片错且缺 BOE 153 B 表**(§3.2)。
7. 帧缓冲放内部 RAM(`epd.c:59` 静态 4000 B);原厂放 PSRAM(`heap_caps_malloc(4000, MALLOC_CAP_SPIRAM)`)。

**复用方式:不可用(仅作地址/命令序参考)。** 模式表本身是对的:0x3c17b488 五个指针 {0:0x4202a5f4, 1:0x42029e9c, 2:0x42029dc4, 3:0x42029cec, 4:0x420299b0}(本次 od 复核),其中 0x4202a5f4 只是 GUI 侧 wrapper,真正执行在 0x42029d9c。
注意:**没有独立的"快速刷"波形**。模式 2(buf)是"整帧 0x13 重传 + 0x12 刷新"(0x42029dc4,`epd.c:157-166`),可当快速重画用,但不换 LUT;模式 3/4 是深睡/唤醒。真正的快慢只由波形(LUT)与窗口决定:BOE 全刷上传 153 B LUT,BOE 局刷用 `0x22 0xFC` 走面板 OTP 波形、不重传 LUT;OED 初始化时写 4 张 55 B LUT。

#### C. 原厂刷新时序(反汇编 + 文档)

- 全刷:原厂 BUSY 等待上限 5 ms × 5000 ≈ 25 s(0x420298a3-0x420298c4)。**2026-11-24 真机严格计时**:面板全刷完成 **1,312 ms**(0x20 后 1,266 ms),两次一致。旧的 `fms=28007 ms` 是 `wait_reaction` 等“回到起始电平”造成的自等待,不是面板慢。
- 局刷:**454 ms**(0x20 后 BUSY 回到低电平),BUSY 确实拉高过。旧的 `pm≈126 ms` 是 120 ms 起始窗口的返回值,不是完成时间。
- **冷启动第一帧必须全刷**:RAM1/RAM2 没有正确初始基准,全用局刷会整屏空白(`docs/E2S_THIRD_PARTY_FIRMWARE.md:16,153`)。badapple 的待机画面就是一次全刷(`badapple/main/playback.c:224-228`)。
- 6 fps(166 ms/帧)远小于 454 ms → 撕裂;2 fps(500 ms)是上限,3 fps(333 ms)也会撕裂。
- **tick 频率不确定**:e2s_re 假设 `CONFIG_FREERTOS_HZ=1000`(`e2s_re/sdkconfig.defaults:44-45` 自注 "assumed"),但这一点没有二进制证据。hello 把原厂 "11 tick" 复位"放宽到 120ms"(`hello_paperang/main/epd.c:138`),无法据此反推(100 Hz 时 11 tick≈110 ms,1000 Hz 时≈11 ms,都说得通)。若原厂是 100 Hz,BUSY 上限就是 5 tick×5000≈250 s 而不是 25 s。阅读器应使用自己的毫秒超时,不要照搬 tick 数。

### 4.2 按键

#### 可用实现:`badapple/main/key.c`

```c
void     key_init(void);        /* GPIO2/4/6/7/8 输入 + 内部上拉 */
bool     key_take_event(void);  /* 消费一次按下沿 */
uint32_t key_mask(void);        /* 当前按住位图,调试/诊断 */
```

- 轮询任务 20 ms 一次,连续 3 次低电平(约 60 ms)算按下,连续 3 次高电平算松开;只上报按下沿(`key.c:18-44`)。任务 pin 到 core 1、prio 3、栈 2048(`key.c:58`)。
- 事件是单个 `volatile bool`,全刷期间按下的键不会丢(`docs/E2S_THIRD_PARTY_FIRMWARE.md:194`)。
- **2026-11-25 真机按键映射**:GPIO7=MENU、GPIO8=OK(实测);GPIO6 暂按 FUNC(原 MENU 位),GPIO2/4 仍为 PREV/NEXT 推断。
- **原厂实际**:5 个脚各自注册独立回调(0x4201de5c 里 5 组 handler 注册,地址 0x4201d950/d964/d978/d98c/d9a0 等),有 500 ms 长按定时器(`e2s_re/components/pkey/key.c:41` 的日志串;0x4201dab0)。原厂扫描周期/去抖参数**没有**从反汇编中恢复,20 ms×3 是 badapple 的重实现(真机可用)。

**复用方式:直接复制。** 若要 5 个键各自语义,需要按 GPIO 拆事件(当前 API 只给"有键按下"的合并事件)。

#### e2s_re `components/pkey`

- `key.c` 全部是空骨架(46 个函数体 TODO,`key.c:1-6`);`pkey_api.c:8` 只打印 `KeyInit`。**不可用。**

### 4.3 电量

#### e2s_re 现状(不可用)

```c
void    battery_check_init(void);       /* battery_check_api.c:8-12,空实现 */
uint8_t battery_check_get_percent(void);/* battery_check_api.c:14-18,硬编码 return 80 */
```

`battery_check.c` 21 个函数体全是 TODO,只保留了日志串/断言串(`battery_check.c:19-48`)。**不能用于产品**。

#### 反汇编恢复出的真实算法(本次复核,可直接实现)

1. `battery_check_init`-等价的校准(0x4202db54-0x4202dbcb):
   - 查 eFuse 校准:`esp_adc_cal_check_efuse`(日志 "Calibration scheme not supported"/"eFuse not burnt, skip software cal");
   - `esp_adc_cal_characterize(ADC_UNIT_1, ADC_ATTEN_DB_11=3, ADC_WIDTH_BIT_12=3, 0, &handle@0x3fca56cc)`(0x4202db99-0x4202dba9)。返回值 1=成功、0=失败(失败就退回未校准)。
2. 每次采样(0x4202dbf8-0x4202dd18):
   - `battery_power(1)` → GPIO45=1,延时 10 tick(0x4202db28);
   - `adc1_config_width(3)`;`adc1_config_channel_atten(0, 3)` → **ADC1_CHANNEL_0 = GPIO1**,11 dB;
   - `adc1_get_raw(0)`;
   - `esp_adc_cal_raw_to_voltage(raw, &handle)` → `mv`;
   - `vbat = mv << 1`(**1:2 分压**,0x4202dc7c);
   - `v = vbat + 25`(固定 25 mV 补偿);若 `vbat > 4300` 打 "read battery voltage error";
   - `pct = 100` 若 `v > 4099`;`0` 若 `v <= 3500`;否则 `pct = (100*v − 350000)/600`(magic 0x1b4e81b5 + `muluh`/`>>6`,0x4202dcf4-0x4202dd12);
   - `battery_power(0)` → GPIO45=0(0x4202dce1)。
3. 低电门限:`app_main` 里 `battery_check_get_percent() < 2` 走低电路径(0x420098f7-0x420098fd;e2s_re `main.c:83` 与此一致)。BLE 文件传输还有 "low power:%d" 门控,但**具体阈值未反汇编确认**(`paperang_ble.c:45` 仅记录日志串)。
4. `(1,11)` 上报的是 `percent*10` 的 LE16(`paperang_ble.c:276-286`)。

**复用方式:不可用(必须按上面重写)。** 注意 GPIO45 在 ESP32-S3 是 VDD_SPI 启动 strapping 脚,输出前要确认板级电路;GPIO5 的充电输入语义也未确认。

### 4.4 SD 挂载、启动顺序、system_time

#### SD(参考重写)

e2s_re `main/sdcard.c`:

```c
esp_err_t sdcard_mount_retry(void);
```

- 结构对:`esp_vfs_fat_sdmmc_mount("/sdcard", &host, &slot, &cfg, &card)`,3 次重试、500 ms 间隔(`sdcard.c:30-60`),与原厂 0x42009b78 一致。
- 引脚/位宽/复位全错(§3.4):把 `SD_SLOT_CLK/CMD/D0` 改成 11/12/13/14/9/10、`width=4`、`flags=SDMMC_SLOT_FLAG_INTERNAL_PULLUP`,并在挂载前做 GPIO34 低 50 ms。
- 挂载失败不要 return(§3.6)。

#### 启动顺序(e2s_re `main/main.c:68-117`,本次用反汇编 0x420098f7-0x42009a9d 复核)

```
gpio_boot_init(); system_time_register();
nvs_init();
battery_check_init(); if (percent < 2) power_manager_low_battery_check();
power_manager_init(); fs_common_init(); wifi_ap_init();
chip banner("This is esp32s3 … 8MB … fw version: 1.3.739");
sdcard_mount_retry();                    // 失败仅打印 FATAL,继续
word_manager_init();
player_init();
epd_init();
key_init();
app_ble_init();
gui_init();
datasync_init();
power_manager_late_init(); ble_late_init(); fs_common_late_init();
heap "total=%d free=%d"
```

对阅读器的建议顺序(去掉词卡/WiFi/音频):NVS(只读)→ 电量 → power manager → SD → EPD → 按键 → BLE → UI。**EPD 与 BLE 必须彼此独立**:badapple 的做法是 BLE 先起(为了连续内存),但面板无论如何都启动(`badapple/main/main.c:365-382`);hello 在 BLE 广播未确认时直接 return(`hello_paperang/main/main.c:296-303`),这是反面教材,与 `docs/E2S_THIRD_PARTY_FIRMWARE.md:15` 的红线冲突。

#### system_time

```c
void system_time_register(void);   /* esp_register_shutdown_handler(cb) */
```

- 反汇编 0x42009820-0x420098a0:用 `esp_timer` 的 64 位差值累加到一个受互斥量保护的 64 位全局(0x3fca2c58),再 `memw` 写入 RTC 寄存器 **0x60008058(低 32)/0x6000805C(高 32)**。
- e2s_re `system_time.c:19-28` 只注册了空回调,精确数学是 TODO;RTC 地址的结论成立,但实现要自己写(用 `esp_timer_get_time()` + RTC 保留内存)。
- 阅读器若不需要"累计使用时长",可以整块删掉。

### 4.5 BLE

#### 可用的最小实现:hello/badapple(NimBLE)

- 栈:**NimBLE**(`CONFIG_BT_NIMBLE_ENABLED=y`,`badapple/sdkconfig.defaults:7`);e2s_re 是 Bluedroid(`sdkconfig:1057`)。
- GATT:一个主服务 `0000ff00-c243-40b9-ac41-a10ba3ad5d1a`,三个特征:
  - FF01 `…ff01` READ|NOTIFY(设备→App)
  - FF02 `…ff02` WRITE|WRITE_NO_RSP(App→设备)
  - FF03 `…ff03` READ|NOTIFY(ready/credit)
  (`badapple/main/main.c:25-31,120-138`;hello 相同)
- 广播:flags + 完整设备名放 adv(共 17 B),128-bit 服务 UUID 放 scan response(18 B);间隔 25–50 ms(`main.c:273-290`)。广播名 `Paperang_E2S`(`badapple/main/demo_config.h:12-13`)。
- MTU:`CONFIG_BT_NIMBLE_ATT_PREFERRED_MTU=247`;分片 `mtu-3`(244 B)(`badapple/sdkconfig.defaults:13`;`badapple/main/main.c:51-55`)。
- ready 通知:FF01 与 FF03 都订阅后发 `{1,3}` 与 `{2, (mtu-3) LE16}`(`main.c:74-83`)。
- 连接/广播状态机:所有 GAP start/stop 只在 host 任务;其他任务只 `ble_npl_eventq_put`;断连/ADV_COMPLETE 重新入队;`ble_service_tick()` 每 5 s 只读检查 + 入队(`main.c:147-188,199-249,251-300`)。广播失败会重试(`main.c:292-295`)。
- **A5 恢复服务**:`a5_server.c` 实现握手 `(1,23)` 回显 + `(1,2)/(1,7)/(1,8)` + `(1,240)` 预检 + `(1,19)` 切槽;badapple 额外有 `(1,241)` 诊断、`(1,242)` 强制全刷、`(3,1)/(3,2)` 只读 flash。它**不接收** `(2,x)` 上传。

API(`badapple/main/a5_server.h:6-8`,hello 相同):

```c
void a5_server_init(void (*notify)(const uint8_t *data, uint16_t len));
void a5_server_disconnect(void);
bool a5_server_on_ble_write(const uint8_t *data, uint16_t len);
```

**复用方式:直接复制。** 注意握手是"回显 challenge + `HELLO PAPERANG DEMO` marker"的简化版(`a5_server.c:305-330`),不是原厂的 `md5(rand+key)`;刷机/恢复流程实测可用,但如果 App 将来校验 md5 就会失败。

#### e2s_re 的 Bluedroid 实现(参考重写,有 bug)

API(`components/ble/e2s_ble.h:14-26`):

```c
void     app_ble_init(void);
uint16_t app_ble_get_mtu(void);
bool     app_ble_is_connected(void);
void     app_ble_notify_ff01(const uint8_t *data, uint16_t len);
void     app_ble_notify_ff03(const uint8_t *data, uint16_t len);
void     paperang_ble_input(const uint8_t *data, uint16_t len);
void     paperang_ble_on_connect(void);
void     paperang_ble_on_disconnect(void);
```

- 值:本地 MTU 185、连接后 1 s `esp_ble_gatt_set_local_mtu(517)`、再 1 s 发 FF03 `[01 03]` + `[02 (mtu-3) LE16]`(`app_ble.c:77-104`)。
- GATT 表 10 条属性、FF02 props 0x0C、FF01/FF03 props 0x10、value perm 0x11/max 500(`e2s_gatt_db.h:17-52`),来自 DROM 0x3C16C1F8。
- **Bug**:`e2s_gatt_db.h` 的 4 个特征值都用 `ESP_UUID_LEN_16` 指向 16 字节 128-bit UUID(第 69/83/96 行),实际会登记成 16-bit UUID 0x5d1a 等。若复用,必须改 `ESP_UUID_LEN_128`。
- **Bug**:CRC 初值 0(§3.3)。
- A5 命令语义最全(mp 握手、NVS sn/key/bind、OTAx 触发、maxLen 4114、maxCache、fileType 路由),适合当"协议字典",不适合当可运行服务器。

#### 阅读器自定义 GATT 服务样板

最佳样板是 **badapple `main.c` 的 `gatt_svcs` + `chr_access_cb`**:一个服务 + 3 个特征 + ready/credit 通知 + 写回调直接喂协议解析器。想加"整本 TXT 上传"时,沿用 FF02 写入口 + `mtu-3` 分片 + 自己加 offset/CRC 即可;不要改 UUID(否则官方 App 扫不到,恢复通道也断)。

### 4.6 LVGL 与显示端口

- **LVGL v8.2.0 已完整 vendor 在 `e2s_re/components/lvgl/lvgl/`**(commit 0b5a1d4,`components/lvgl/VENDOR.md`),`lv_conf.h` 按原厂指纹配置:`LV_COLOR_DEPTH=1`、`LV_MEM_CUSTOM=0`、`LV_MEM_SIZE=32K`、`LV_USE_QRCODE=1`、仅 `LV_FONT_MONTSERRAT_14=1`(`lv_conf.h:41-43,60,86,97`)。**可直接复制。**
- **显示端口 `components/pgui/pgui_api.c` 是真实实现**:`gui_init()` 建 8000 px 单缓冲、250×128、full_refresh=1、flush_cb 按 `idx=y*250+x` 打包到 `epd_get_framebuffer()`,只在最后一个 band 调 `epd_refresh(0)`,并调用 `lv_disp_flush_ready()`(`pgui_api.c:87-169`)。
  - 代码里有很有价值的评审注记:pristine v8.2 会因 `8000 < 250*128` 把 full_refresh 静默清 0,实际是 32 行/band(`pgui_api.c:28-36`)。
  - 但它依赖的 `epd_refresh(0)`/引脚是错的(§4.1B),且 `ver_res=128` 有 6 行不可见。
  - **复用方式:参考重写**(把 flush 目标换成 §4.1A 的 `epd_draw_ex()`,或保留打包但改用正确引脚/SPI/LUT;分块时注意只有最后一个 band 才整屏刷新)。
- 若不想上 LVGL:hello/badapple 的 `draw_text()` + 8×16 字模 + 直接 `epd_draw_ex()` 是最小可行路径(`badapple/main/playback.c:167-209`)。

### 4.7 FreeType

- `e2s_re/components/freetype2/` 已 vendor **FreeType 2.13.2**(上游 tag VER-2-13-2),并做了原厂 18 模块裁剪 + smooth-lcd/lcdv 回移植(`components/freetype2/CMakeLists.txt:1-17`;`VENDOR.md:24-73`)。
- `ft_memory.c/ft_memory.h` 提供 PSRAM 内存接线:

```c
FT_Memory ft_memory_init(void);   /* 16B FT_MemoryRec,全部 heap_caps(MALLOC_CAP_SPIRAM) */
FT_Error  ft_library_init(void);  /* 静态 FT_LibraryRec + 18 模块 */
```

- **构建状态**:本次确认 `build/esp-idf/freetype2/...` 下有 **38 个 .obj**(含 `ft_memory.c`、全部 `freetype/src/*.c`),说明组件参与了编译;但 `build/e2s_re.map` 里 `FT_Init_FreeType`/`FT_Add_Module`/`ft_memory_init` 均为 0 引用,`libfreetype2.a` 只在链接命令行出现 → **当前固件没有把 FreeType 链进 bin**。
- `diskio.c` 仍是骨架(60 个函数 TODO,`diskio.c:1-6`),没有 TTF 容器解析、没有 `FT_New_Memory_Face`/渲染代码。`VENDOR.md:253` 也明确"未运行 idf.py build"(本次确认后来已编译,但未做端到端渲染)。
- **复用方式:参考重写**。要用它显示中文,至少还要写:字体加载(flash/SD → 内存 face)、`FT_Set_Pixel_Sizes`、`FT_Load_Char`、1bpp 位图 blit 到面板缓冲。工作量和 RAM 都不小。

### 4.8 字体资产

| 资产 | 位置/格式 | 状态 | 复用 |
|---|---|---|---|
| `font8x16` ASCII | `hello_paperang/main/font.c` / `badapple/main/font.c`(两份 `diff` 相同,108 行) | 96 字形(0x20–0x7F),每字形 16 B,MSB first,1=亮 | 直接复制;只有 ASCII |
| LVGL `montserrat_14` | `e2s_re/components/lvgl/lvgl/src/font/lv_font_montserrat_14.c`;`lv_conf.h:97` 开启 | 4bpp 抗锯齿,ASCII+少量符号 | 直接复制;中文不行 |
| 原厂 5 个位图字体 | 原厂 app DROM:`lv_font_12 @0x3C16EE18`(lh12,含中文)、`lv_font_16 @0x3C174CD0`(lh16,含中文)、`lv_font_18 @0x3C177550`(数字)、`lv_font_22 @0x3C177708`(数字)、`lv_font_30 @0x3C177950`(ASCII);均为 LVGL `lv_font_t` + fmt_txt,1bpp | 本次在 `dump/e2s_flash_8mb_r5.bin` 里逐字段验证:get_glyph_dsc=0x420d1a48、get_bitmap=0x420d1a08、line_height/base_line 与 `VERSIONS.md:95` 一致 | **不可直接用**:数据是编译进原厂 app 的结构体/位图,需要写提取器把它们转成 C 数组;没做 |
| `Zpix-Regular` TTF | dump **0x480040**,长度 **392,196 B**(FTF 头在 0x480000) | 本次 `dd` 提取到 /tmp 后 `file` 识别为 TrueType 11 表;fontTools 读出 **2190 个 cmap 项**,含"中/文/A",name=`Zpix Regular` | **可直接复制/提取**:OFL 许可;是最省事的中文来源 |
| Alibaba PuHuiTi Light 子集 1 | dump **0x4DFC84**,81,984 B | FTF 头 + 标准 TTF(`od` 验证 sfnt 00 01 00 00) | 数据可用,但**商业字体,不可公开分发**(`FLASH_DUMP_FINDINGS.md:123`) |
| Alibaba PuHuiTi Light 子集 2 | dump **0x4FB7E4**,146,108 B | 同上 | 同上 |
| FTF 容器 | `0x40` B 头:`"FTTF"` + 名字(0x04)+ u32 TTF size(0x24)+ 0xFFFFFFFF(0x28)+ 0x00010001(0x2C)+ u32(0x30,与 Zpix 的 2190 一致)+ TTF 从 0x40 开始 | `dump/FLASH_DUMP_FINDINGS.md:107-118`;本次 od 逐字段核对 | 读取原厂字库时需要 |
| 位图缓存 | dump **0x520000–0x528884**(约 34.6 KB,1bpp 图案) | `FLASH_DUMP_FINDINGS.md:120-121` | 未确认用途,不要依赖 |

### 4.9 A5 / 刷机 / 恢复工具

- `tools/e2s_ble_flash.py`:`scan/info/restore/flash/dump/full`,`--force` 跳过低电量门限(`docs/E2S_THIRD_PARTY_FIRMWARE.md:251`)。
- `tools/e2s_flash_check.py`:只读校验分区表/otadata/app 描述符(`docs/E2S_THIRD_PARTY_FIRMWARE.md:252`;本次未运行)。
- `tools/A5ProtocolTest.java` + `tools/mock_e2s.py`:协议差分测试(`e2s_re/docs/RECOVERY_MAP.md:91-93`)。
- 这些是**开发/恢复流程资产**,不是固件代码;阅读器项目应保留"自定义 → (1,240) 回原厂 → 原厂 OTA"的通道(`docs/E2S_THIRD_PARTY_FIRMWARE.md:71-72`)。

---

## 5. 已被真机验证的事实(只列有出处的)

来源:`docs/E2S_THIRD_PARTY_FIRMWARE.md`(纯软件经验总结)与 `badapple/README.md`。

1. 全刷完成 **1,312 ms**(0x20 后 1,266 ms,BOE,两次一致)。旧值 28,007 ms 是 `wait_reaction` 的自等待。
2. 局刷完成 **454 ms**(BOE,0x20 后 BUSY 回低)。旧值 ≈126 ms 是 120 ms 起始窗口的返回值。
3. **冷启动第一帧必须全刷**;全用局刷会整屏空白(`:16, :153`)。
4. 6 fps(166 ms/帧)会撕裂;2 fps(500 ms/帧)是上限,3 fps 也会撕裂(`:157-159`)。
5. 局刷序列:`0x24` 当前帧 → `0x22 0xFC` → `0x20` → 等 → `0x26` 回写同一帧(`:149`,`badapple/README.md:12-14`)。
6. 待机/按键状态机:待机广播开;按键 → 关广播、2 fps 播放;再按 → 停、全刷清屏、开广播(`:195-199`)。
7. 按键 GPIO **2/4/6/7/8**,低电平按下(`:192`);"20 ms 轮询 / 连续 3 次"是 badapple 的实现(`badapple/main/key.c:18-44`),原厂扫描参数未从反汇编恢复。
8. 面板族:复位后 BUSY=0 → BOE;BUSY=1 → 读 0x70 按 ID 分 OED/GD(`:147`)。
9. 刷机只走原厂 OTA 写非运行槽;恢复只改 otadata;`ota_1` 永远不动(`:11-12, :68-72`)。
10. PHY 校准坑:自定义固件冷启动做全量校准,可能出现"host 说广播成功、射频不发波";原厂从 NVS `phy` 读校准(`:124-137`)。
11. "面板当日志":启动阶段码 `BLE:1..6`、`(1,241)` 诊断、`fr=ok,c=1314,fps=6`、按键掩码(`:120, :233`;`badapple/main/main.c:44-49`)。
12. badapple 实测:472,704 B app;438 帧 @2 fps;每 96 帧或变化 >90% 全刷,整段 6 次全刷(`badapple/README.md:10-13,26`)。

---

## 6. 中文显示:最省事方案

按"工作量 / 风险 / 效果"排序:

### 方案 A(推荐,最省事):把 Zpix 12px 预转换成 LVGL 1bpp 字库
1. 从 dump 提取(本次已验证有效):
   ```sh
   dd if=dump/e2s_flash_8mb_r5.bin of=zpix.ttf bs=1 skip=$((0x480040)) count=$((0x5FC04))
   file zpix.ttf        # → TrueType Font data, 11 tables
   ```
   Zpix 只有 **2190 个 cmap 映射**(本次 fontTools 读出,与 FTF 头 0x30 字段一致),按 16px 1bpp 每字约 40 B 估,全量约 88 KB,加 cmap 约 100–130 KB,完全可以编进 2 MB 的 app。
2. 在 PC 上用 `lv_font_conv`(LVGL 官方工具)生成 `lv_font_t` C 数组:`--bpp 1 --size 16 --font zpix.ttf --range 0x20-0x7E --symbols <书里出现的汉字>`;若要全量,用 `--range 0x4E00-0x9FFF` 会远超 Zpix 实际字形,应改为按 cmap 导出。
3. 优点:运行时不需 FreeType/PSRAM,渲染确定、速度快、12/16 px 像素字最清晰;缺点:字库固定,生僻字/新书要重新生成。
4. 许可:Zpix 是 OFL,可随固件分发;PuHuiTi 是商业字体,不要打包。

### 方案 B(最忠实):提取原厂 12px/16px 中文位图字体
- 位置已知且本次验证:`0x3C16EE18`(12px,含中文)、`0x3C174CD0`(16px,含中文),都是 LVGL v8.2 `fmt_txt` 1bpp 结构(`VERSIONS.md:95`)。
- 需要写一个提取器:解析 `lv_font_t` → `lv_font_fmt_txt_dsc_t` → `glyph_dsc[]`/`glyph_bitmap[]`/cmap,输出 C 数组。工作量中等,但效果与原厂 UI 完全一致,且体积极小。
- 目前仓库里**没有**这个提取器,也没人做过。

### 方案 C(最灵活,最重):FreeType 2.13.2 + Zpix/任意 TTF
- 组件已就位并可编译(38 个 obj),但未链接、无渲染代码、无字体加载器(§4.7)。
- 需要:把 TTF 放进 flash(0x480040 或 app 内)、`ft_memory_init()` + `ft_library_init()`、`FT_New_Memory_Face`、按字号缓存 glyph。PSRAM 是否可用/多大未验证,是这条路的硬前提。
- 只有在"用户可换字体/需要任意字号"时才值得。

**建议:阅读器 MVP 走方案 A;若要求与原厂观感一致,再补方案 B。** 方案 C 留给后续。

---

## 7. 阅读器固件必须遵守的安全红线

1. **只写 app 槽**:不写 bootloader、分区表、NVS、phy_init;不是恢复操作时不动 otadata;`ota_1`(0x210000–0x400000)永远不碰(`docs/E2S_THIRD_PARTY_FIRMWARE.md:11-12`;`hello_paperang/README.md:52`;`badapple/README.md:28`)。
2. **不用 `idf.py flash`**;产物是 app-only 镜像,经原厂 OTA 或 `tools/e2s_ble_flash.py` 写入(`hello_paperang/README.md:50-52`)。
3. **不初始化/擦除原厂 NVS**;`sn`/`key`/`bind`/`cal_data`/`cal_mac` 是设备身份,不打印、不外传(`docs/E2S_THIRD_PARTY_FIRMWARE.md:137`;`dump/FLASH_DUMP_FINDINGS.md:76-86`)。
4. **冷启动第一帧必须全刷**,否则整屏空白、会被误判为"没启动"(`docs/E2S_THIRD_PARTY_FIRMWARE.md:16,153`)。
5. **app_main 不能因 BLE 失败而 return**;面板/UI 必须独立启动(`docs/E2S_THIRD_PARTY_FIRMWARE.md:15`;正面例子 `badapple/main/main.c:365-382`,反面例子 `hello_paperang/main/main.c:296-303`)。
6. **SD 失败不能中止启动**;显示"无卡/错误"并保持 BLE 恢复通道(原厂行为见 §3.6)。
7. **不要照抄 e2s_re 的 EPD 引脚/LUT/BUSY 等待**(§3.1/§3.2/§4.1B);GPIO46/45 是 strapping 脚,不要想当然驱动。
8. **当前全刷实现会阻塞 ~28 s(真实完成 1.31 s)**:不要放在被 5 s 任务看门狗监控的任务里裸等;用 `vTaskDelay` 轮询或拆到独立任务,并在这段时间保持 BLE 事件可处理。修等待逻辑后阻塞可降到 ~1.5 s。
9. **局刷只对 BOE 实现**;OED 会回退全刷。上线前用 `(1,241)` 的 `fam=` 确认目标批次,否则翻页走全刷(BOE 实测 1.31 s;OED 未测)。
10. **A5 协议 CRC 用 896963873**,不要用 e2s_re 的 init=0(§3.3),否则官方 App 的帧全部 CRC 失败。
11. **恢复通道要保留**:至少实现 `(1,240)` 预检 + `(1,19)` 切回原厂槽,并在切换前校验目标槽是有效 app(magic 0xE9 + desc magic 0xABCD5432 + SHA-256),不要实现 `(2,x)` 写入(`docs/E2S_THIRD_PARTY_FIRMWARE.md:71-72`;`badapple/main/a5_server.c:140-201`)。
12. **深睡要谨慎**:hello/badapple 的 `epd_deep_sleep()` 是空操作(`badapple/main/epd.c:481-484`);原厂是 `0x07+0xA5`(0x42029cec),唤醒路径未在样例中验证。没验证前不要进 deep sleep,否则可能黑屏且无法唤醒。

---

## 8. 未验证 / 已知坑 / 风险

1. **PSRAM 容量未实测**:2 MB 仅来自设备 JSON;原厂确实从 SPIRAM 分配(LVGL 32 KB、FreeType、EPD 4000 B),但"2 MB"没有硬件证据。FreeType/大缓冲方案必须先 `esp_psram_get_size()` 验证。
2. **BUSY 等待逻辑已定位**:2026-11-24 真机严格计时,BOE 全刷完成 **1,312 ms**、局刷 **454 ms**,BUSY 极性为高=忙/低=空闲。旧的 28 s 是 `wait_reaction` 等“回到起始电平”的自等待。正常路径仍用旧等待,需要改成“等低电平/等拉高后回低”。
3. **OED 无局刷**:若目标机 `fam=3`,当前驱动每次翻页全刷(BOE 实测 1.31 s;OED 未测)。需要在 OED 上补 UC8151 局刷序列(LUT 已具备 0x20/0x22/0x23/0x24)。
4. **SD 引脚结论来自反汇编,未在真机跑过**:11/12/13/14/9/10 + width 4;GPIO34 低=使能、挂载期间保持低、卸载后置高(2026-11-25 从 0x42009b34/0x42009ce5 复核)。仓库里没有其它代码用过这组值;第一次上机要先用 `sdmmc_card_print_info` 验证。
5. **电池算法来自反汇编,未实测**:ADC1_CH0=GPIO1、÷2、3500–4100 mV、GPIO45 使能。GPIO45 是 VDD_SPI strapping;GPIO5 的充电语义未确认。分压比/校准是否随批次变化未知。
6. **按键物理映射**:GPIO7=MENU、GPIO8=OK 已实测;GPIO6 暂按 FUNC,GPIO2/4 仍是 PREV/NEXT 推断。阅读器 `board.h` 已按此更新。
7. **中文位图字体未提取**;FreeType 路径无渲染代码、未链接、无 PSRAM 验证。方案 A 需要 PC 端 `lv_font_conv`,仓库里没有现成生成脚本。
8. **A5 握手是简化版**:hello/badapple 回 `HELLO PAPERANG DEMO`,不是 `md5(rand+key)`;目前 App 流程可用,但属于未验证的兼容性假设。
9. **e2s_re 的 A5 实现有 CRC bug、GATT 表有 UUID 长度 bug**(§3.3/§4.5),不能直接当服务器跑。
10. **e2s_re 的 `paperang_ble.c` 依赖外部桩函数**:`word_manager_book0_size()`、`word_manager_get_active_slot()`、`battery_check_get_percent()`(第 270/278/347 行),这些在 e2s_re 里本身也是骨架;单独抽出 BLE 要一并替换。
11. **字库区 0x480000–0x528884 在分区表外**:如果将来用原厂 OTA 升级原厂固件,理论上不会碰;但如果有人整片擦写会丢字库。不要把它当"自己的"区域写。
12. **app 上限 2 MB**:LVGL+FreeType+全量中文字库+书缓存可能逼近上限;badapple 470 KB、e2s_re 1.48 MB 是参考。超了要么裁字库,要么把字库放裸 flash/SD。
13. **`hello_paperang/README.md:104` 的 4 MB 说法是错的**;不要据此选 flash 参数。
14. **e2s_re 全刷等待可能长达 50–200 s**(§4.1B 第 3 条),若有人直接拿它的 `epd_refresh(0)` 会看起来"死机"。
15. **`word_manager`/`WORD_MANAGER.md` 是词卡业务模型**(book0/1、ctb/csb/cdb/crb),与"读 TXT/EPUB"无关;不要为阅读器复用这套文件格式(`e2s_re/docs/WORD_MANAGER.md:51-77`)。

---

## 9. 证据索引(本次实际执行过的检查)

### 9.1 文件/文档
- `firmware/e2s_re/components/epd/epd.c`、`epd.h`、`epd_luts.h`
- `firmware/e2s_re/components/pkey/key.c`、`pkey_api.c`
- `firmware/e2s_re/components/battery_check/battery_check.c`、`battery_check_api.c`
- `firmware/e2s_re/main/main.c`、`sdcard.c`、`system_time.c`、`gpio_boot.c`
- `firmware/e2s_re/components/ble/app_ble.c`、`e2s_ble.h`、`e2s_gatt_db.h`、`paperang_ble.c`
- `firmware/e2s_re/components/freetype2/{CMakeLists.txt,diskio.c,ft_memory.c,ft_memory.h,VENDOR.md}`
- `firmware/e2s_re/components/lvgl/{lv_conf.h,VENDOR.md}`、`components/pgui/pgui_api.c`
- `firmware/e2s_re/{sdkconfig.defaults,sdkconfig.defaults.v44,sdkconfig,partitions_re.csv,README.md}`
- `firmware/hello_paperang/{README.md,main/epd.c,main/epd.h,main/epd_lut_stock.h,main/main.c,main/font.c,main/demo_config.h,sdkconfig.defaults}`
- `firmware/badapple/{README.md,main/epd.c,main/epd.h,main/epd_lut_stock.h,main/key.c,main/key.h,main/main.c,main/playback.c,main/a5_server.c,main/a5_server.h,main/font.c,main/demo_config.h,sdkconfig.defaults}`
- `docs/E2S_THIRD_PARTY_FIRMWARE.md`、`docs/FLASH_DUMP_FINDINGS.md`
- `firmware/e2s_re/docs/{RECOVERY_MAP.md,VERSIONS.md,WORD_MANAGER.md}`

### 9.2 命令(均已在本次执行)
```sh
# 分区表/otadata/app desc/字体头:逐字节
od -A x -t x1z -j $((0x8000))  -N 160 dump/e2s_flash_8mb_r5.bin
od -A x -t x1z -j $((0xf000))  -N 64  dump/e2s_flash_8mb_r5.bin
od -A x -t x1z -j $((0x210020)) -N 64 dump/e2s_flash_8mb_r5.bin
od -A x -t x1z -j $((0x480000)) -N 96 dump/e2s_flash_8mb_r5.bin
od -A x -t x1z -j $((0x4FB7A4)) -N 80 dump/e2s_flash_8mb_r5.bin

# 原厂反汇编定点复核(xtensa,adjust-vma=0x42000020,seg3=IROM)
OBJ=~/.espressif/tools/xtensa-esp-elf/esp-14.2.0_20260121/xtensa-esp-elf/bin/xtensa-esp32s3-elf-objdump
$OBJ -D -b binary -m xtensa --adjust-vma=0x42000020 \
     --start-address=0x4202a828 --stop-address=0x4202a998 \
     tools/fw_ext/seg3_0x42000020.bin          # EPD GPIO/SPI 初始化
# 同法复核:0x420298c8(复位)、0x420298f4(pre_cb/DC=39)、0x42029a14(cmd/CS=40)、
#          0x420298a3(BUSY 等待)、0x4202a998(epd_init)、0x42009b34/0x42009b78(SD)、
#          0x4202db28/0x4202dbf8(电量)、0x4201de5c(KeyInit)、0x42009820(system_time)、
#          0x420098f7(低电门限)、0x42009ac4(SD 失败继续)

# 模式表 0x3c17b488(5 项)与 DROM 字体结构
od -A x -t x4 -j $((0x3c17b488-0x3c160020)) -N 40 tools/fw_ext/seg0_0x3C160020.bin
python3 -I /tmp/e2s_probe/font.py     # 读 dump 0x210020+DROM 偏移,打印 5 个 lv_font_t 字段

# Zpix 提取与校验
dd if=dump/e2s_flash_8mb_r5.bin of=/tmp/e2s_probe/zpix.ttf bs=1 skip=$((0x480040)) count=$((0x5FC04))
file /tmp/e2s_probe/zpix.ttf          # TrueType Font data, 11 tables
python3 -I -c "from fontTools.ttLib import TTFont; ..."   # cmap=2190,name=Zpix Regular

# LUT 逐字节比对
python3 -I -c "解析 epd_luts.h / epd_lut_stock.h 并比较"   # 4 张 55B 表 == e2s_re 表 [1:]

# 组件/构建状态
find build -path "*freetype2*" -name "*.obj" | wc -l      # 38
grep -c "FT_Add_Module" build/e2s_re.map                  # 0(未链接)
ls -l build/e2s_re.bin build_v44/e2s_re.bin               # 1,482,272 / 1,299,872
ls -l ../badapple/build/badapple_e2s.bin                  # 472,704
ls -l ../hello_paperang/build/hello_paperang.bin          # 485,664
diff -q hello_paperang/main/epd_lut_stock.h badapple/main/epd_lut_stock.h   # 相同
diff -q hello_paperang/main/font.c badapple/main/font.c                     # 相同
```

### 9.3 本次未运行
- `idf.py build`(任何工程)、`idf.py flash`、`esptool`、真机串口/BLE 测试。
- `tools/e2s_flash_check.py`、`tools/e2s_ble_flash.py`。
- FreeType 端到端渲染、LVGL 在真机上的 flush 验证、SD 4-bit 挂载、ADC 实测。
- 原厂 1.3.739 位图字体的 C 数组提取。
