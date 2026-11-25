# E2 1.3.759 与 E2S 1.3.739 固件差异

> 2026-11-26。对象是两个 **app-only** 镜像；没有 E2 真机，未运行任何一侧。
> 结论按【观测】（直接字节/字符串/指令）、【推断】（由观测推出，未运行）、
> 【未知】三级标注。

## 0. 结论

E2 固件 ≈ E2S 固件去掉三块源码：

- `components/backlight/aw2023.c`
- `components/backlight/backlight_ctrl.c` —— 背光
- `components/headphone/headphone_check.c` —— 耳机插入检测 + USB-C CC1/CC2 ADC 采样

以及它们绑定的：

- 设置页 `gui_page_set_backlight`（“阅读灯光”）
- NVS 键 `BLBri`
- 产测任务 `ft_backlight_test_task` / `ft_headphone_test_task`
- E2S 多两段内嵌 Opus 音频（合计 21,124 B，占 DROM 差的 91%）

其余硬件配置在固件层一致：EPD 引脚/LUT、按键、GPIO5 充电检测、电池 ADC、
ES8311/I2S、SD/FATFS、Wi-Fi/BLE、RTC、LVGL 字体。

E2 还带一条更新的通用代码线（i2c_bus deinit/refcount、TTF/数据同步 MD5 日志），
E2S 带旧线（`i2c_master_cmd_begin_with_conf`、`fs_*` 调试日志）。这部分是软件代差，
不是硬件差异。

## 1. 样本

| 项 | E2 | E2S |
|---|---|---|
| 文件 | `downloads/paperang_e2-v1_3_759_13a9f.bin` | `downloads/paperang_e2s-v1_3_739_e5938.bin` |
| 来源 | 原厂 OTA 接口，按 E2 SN 查询（SN 不记录） | 原厂 OTA 接口按本机 SN 查询；与设备 dump `ota_1` 逐字节一致 |
| 大小 | 1,998,128 B (0x1E7D30) | 2,026,352 B (0x1EEB70) |
| MD5 | `38c8048da8cedd5bc337e963f35a9041` | `df6d2323da289a4f4d6b8db277fb33ed` |
| SHA-256 | `44c3307331ef25f5428a07b5c08d81d5a246b92e7c26031fab485f4f9a97eb53` | `162e7c552254726f6e231888bd1d0ba09007ced7cc1b83f2276641b225db740e` |
| app 版本字段 | `d3258bdec-dirty` | `c6b2d574b-dirty` |
| 用户可见版本 | **1.3.759** @0x3C163DCC | **1.3.739** @0x3C163DCC |
| project | `paperang_e2` | `paperang_e2` |
| 构建时间 | `Oct 25 2023 10:12:10` | `Oct 26 2023 09:54:47` |
| ELF SHA-256 | `0ee25e0963629d6217a1d6a10ff5a4c6c502e62de77f014839cd64ae23088a0d` | `ebf8355f4faf9c9affc4f9ad68cc7c038566ab2a75f5e392dee175e7682b20ee` |
| 尾部 | pad 15 B + checksum 0xE1 @0x1E7D0F + SHA @0x1E7D10 | pad 11 B + checksum 0xF2 @0x1EEB4F + SHA @0x1EEB50 |

两包的 ESP 段校验（0xEF 起逐字节 XOR）与尾部 SHA-256 均已复核通过；
E2S 包与设备 `ota_1` 的前 0x1EEB44 字节逐字节相同。

## 2. 方法

1. 解析 ESP app 段表，按加载地址切出 DROM/DRAM/IRAM/IROM/RTC；
2. 从两个镜像抽 NUL 结尾的可打印字符串（ASCII + UTF-8），做集合差；
3. 用 `firmware/e2s_re` 已有的 `tools/fw_ext/app_func_map.json` +
   `func_lits_raw.json` 把 E2S 字符串归属到源文件，检查 E2 是否还引用同一字符串；
4. 对 EPD GPIO 初始化、充电检测中断两个函数做 objdump 逐指令比对；
5. 只做静态分析，不运行、不写设备。

复现：`python3 tools/e2_e2s_diff.py`（见 §7）。

## 3. 差异清单

| 能力 | E2 | E2S | 证据等级 |
|---|---|---|---|
| 背光（AW2023 + 控制层） | 无 | 有 | 观测 |
| 耳机插入检测 | 无 | 有 | 观测 |
| USB-C CC1/CC2 ADC 采样/校准 | 无 | 有（ADC2） | 观测 |
| 产测 背光/耳机 | 无 | 有 | 观测 |
| 设置页“阅读灯光” | 无 | 有 | 观测 |
| NVS `BLBri` | 无 | 有 | 观测 |
| 内嵌 Opus 音频 | 1 段 14,321 B | 3 段 35,445 B | 观测 |
| PM 锁 info 日志 / `clean` | 无 | 有 | 观测 |
| `i2c_bus` 代次 | 新（deinit/refcount） | 旧（`_with_conf`） | 观测 |
| TTF/数据同步 MD5 日志 | 有 | 无 | 观测 |
| `fs_*` 文件操作日志 | 无 | 有 | 观测 |
| EPD 引脚/LUT、按键、GPIO5 充电、电池 ADC、ES8311/I2S、SD/FATFS、Wi-Fi/BLE、RTC、字体 | 相同 | 相同 | 观测 |

### 3.1 耳机 + USB-C CC（除背光外最大的实质差异）

E2S 有源文件 `./components/headphone/headphone_check.c`，E2 **没有这个文件路径**。
随之消失的字符串/能力（E2 计数全为 0）：

- `headphone_insert_detect: %d` —— 耳机插入检测
- `cc1 cc2 voltage: %d mV %d mV` / `PULLUP cc1 cc2 voltage` / `cc cali data: %d mV (%d %d)`
- `adc2_config_channel_atten(CC1_ADC_CHANNAL, CC_ADC_ATTEN)` /
  `adc2_config_channel_atten(CC2_ADC_CHANNAL, CC_ADC_ATTEN)` / `adc2_get_raw` / `ADC2 Atten Err`
- `pp_ft_headphone_test: step %d` / `data_len err`

结论：

- 【观测】E2 没有耳机插入检测，也没有 CC1/CC2 的 ADC2 电压采样/校准。
- 【推断】E2S 通过 USB-C CC 做耳机/附件检测；E2 的 USB-C 只剩充电。
- 【未知】E2 是否有物理耳机口（没有检测代码不等于没有座子）。

**充电检测本身没变**：两边都是 **GPIO5**（pin mask `0x20`）中断，
`battery_charge_ping_irq_register` 逐指令一致：

- E2S `l32r ... (0x20)` @0x4202E07B，`movi a10,5`（gpio_isr_handler_add）@0x4202E0A7
- E2 `l32r ... (0x20)` @0x4200BD77，`movi a10,5` @0x4200BDA4

### 3.2 背光（已知）

E2S 独有：

- 源文件 `./components/backlight/aw2023.c`、`./components/backlight/backlight_ctrl.c`
- 字符串 `aw2023`（28 次）、`AW2023`（8 次）、`backlight`（18 次）
- NVS 键 `BLBri` 与 `AppGuiGet/SetBacklightBrightness`
- 设置页 `gui_page_set_backlight`（+`_init`/`_deinit`）与文案 `阅读灯光`
- 产测 `ft_backlight_test_task`

E2 侧全部为 0。

### 3.3 产测

- E2S：`ft_backlight_test_task`（3 次）、`ft_headphone_test_task`（2 次）、
  `MM_FT_SPEAKER_TEST`（2 次，带 `opus_id`）。
- E2：没有上述三项；`ft_play_test_task`、`ft_deepsleep_test_task`、`ft_diskio_init`
  两边都有，key/lcd 产测两边都有。

【推断】E2 的音频播放能力仍在（`ft_play_test_task`、`VoiceVol/VoicePlay`、
ES8311/I2S 都在），只是没有 E2S 那条扬声器/耳机/背光产测路径。

### 3.4 UI / NVS

- GUI 页：E2S 多 `gui_page_set_backlight`、`gui_page_set_backlight_init`、
  `gui_page_set_backlight_deinit`；E2 没有。E2 有一个独立的 `gui_page_common`
  字符串而 E2S 没有，但两边都有 `gui_page_common_page_factory_test_*`，属于命名/字符串布局差异，不是页面缺失。
- 文案：E2S 有 `阅读灯光`，E2 没有；`单击` 两边都有（各 2 次）。
- NVS：27 个已知键里 **只差 `BLBri`**。`sn/key/bind/cal_*/bookCfg/bookAvai/
  markIdx0/1/VoiceVol/VoicePlay/...` 全在两边。

### 3.5 功耗/刷新路径（易误判：锁 API 两边都有）

两边**都有** `pm_light_sleep_lock_*` API；E2 有 create/acquire/release/hold 的
**错误**日志，E2S 在此基础上多出 info 级成功日志：

- E2S-only：`pm_light_sleep_lock_create: id=%d name=%s`、
  `..._acquire: id=%d name=%s`、`..._release: id=%d name=%s cnt=%d %p`、
  `..._hold: id=%d name=%s cnt=%d`、`..._clean: id=%d name=%s cnt=%d %p`
- E2 没有 `pm_light_sleep_lock_clean` 这个字符串（clean 可能是 E2S 新增）。

EPD 刷新路径里的 acquire/release **info 日志**只在 E2S 出现；E2 可能仍调用锁，
只是不打日志。

【推断】锁机制两边都有；E2S 多了 info 日志和 `clean`。不要据此认为 E2 不做
light-sleep 保护。

### 3.6 内嵌 Opus 音频

| 镜像 | VMA | 长度 | Ogg 页数 | MD5 |
|---|---|---|---|---|
| E2 | 0x3C17BA54 | 14,321 B | 4 | `80fd534a27b97d2580036b91c2a60b77` |
| E2S | 0x3C17D034 | 7,692 B | 3 | `aa21f81c28ea7eeebbc062f925567671` |
| E2S | 0x3C17EE40 | 14,321 B | 4 | `80fd534a27b97d2580036b91c2a60b77` |
| E2S | 0x3C182634 | 13,432 B | 4 | `d88e72bb9940845994b051a04be15232` |

E2S 比 E2 多 7,692 + 13,432 = **21,124 B**，占 DROM 差 23,112 B 的 **91.4%**。
E2 那一段与 E2S 中间那段**逐字节相同**。

【推断】多出的两段是产测/提示音（E2S 有 speaker/headphone/backlight 产测）；
具体哪段对应哪个测试未验证。

### 3.7 软件代差（不是硬件）

- `i2c_bus`：E2 有 `i2c_bus_delete`、`i2c_driver_deinit`、`i2c not inited`、
  `i2c%d is also handled by others ref_counter=...`；E2S 有
  `i2c_master_cmd_begin_with_conf`、`reinit error`、`aw2023_i2c_init/deinit`。
- MD5：E2 有 `caculate_ttf_font_md5`（2 次）、`caculate:md5_code:`、
  `DATA-SYNC:current md5_code:`；E2S 完全没有。
- `fs_*` 日志：E2S 多 6 条 info 日志（fopen/fclose/opendir/closedir/try_mkdir/unlink）；
  E2 的函数还在（`fs_fopen` 仍出现 5 次），只是不打这些日志。
- 调试日志：E2 多 `[zhangj] e: now_time ...`、`[zhangj] e: study_date ...`；
  E2S 多 `BAT_BAR_UPDATE: battery percentage=...`。
- IDF 代际：两边都含 v4.4 系路径（`/IDF/components/esp_adc_cal/esp32s3/esp_adc_cal.c`、
  `/IDF/components/driver/i2c.c`、`/IDF/components/bt/controller/esp32s3/bt.c`），
  倾向同一 IDF 大版本；app 描述里的 `idf` 字段是 app 的 git 版本，不是 IDF 版本。

【推断】E2（1.3.759）这条线比 E2S（1.3.739）的通用组件更新；
E2S 是另一条产品分支，不是简单的新旧关系。

### 3.8 型号 / 认证

| 项 | E2 | E2S |
|---|---|---|
| BLE 名 | `Paperang_E2` | `Paperang_E2S` |
| CMIIT ID | `2022DP8563` | `2022DP13248` |
| 用户可见版本 | 1.3.759 | 1.3.739 |
| A5 设备 JSON `DevName` | `Paperang_E2` | `Paperang_E2S` |

A5 设备 JSON 模板其余字段完全一致（`RomPosition/RomSize/DevSn/WiFiSupport/
BleSupport/OtaElapsedTime/Book`）。

## 4. 体积账

| 类型 | E2 | E2S | 差 |
|---|---|---|---|
| DROM | 469,756 | 492,868 | **-23,112** |
| DRAM | 19,692 | 19,692 | 0 |
| IRAM | 88,112 | 88,112 | 0 |
| IROM | 1,410,264 | 1,415,396 | **-5,132** |
| RTC | 10,168 | 10,152 | +16 |
| 段负载合计 | 1,997,992 | 2,026,220 | -28,228 |
| 文件 | 1,998,128 | 2,026,352 | -28,224 |

- IRAM 两边总量相同（88,112 B），只是段边界/加载地址不同
  （E2: 0x40378000+0x8800 / 0x40380800+0xD030；
  E2S: 0x40378000+0x2DB8 / 0x4037ADB8+0x12A78），按地址逐段比会误判。
- 文件差比段负载差少 4 B，是 checksum 前填充对齐不同（E2 pad 15 B，E2S pad 11 B）。
- DROM 差 23,112 B 里 21,124 B 是两段多出的 Opus；剩余约 1,988 B 是
  背光/耳机/CC 的字符串与表。
- IROM 差 5,132 B 是背光 + 耳机/CC + 背光页 + 产测任务的代码。

OTA 槽 0x1F0000（2,031,616 B）：E2 余 33,488 B，E2S 余 5,264 B。

## 5. 确认相同的部分

- **EPD**：E2S `epd_hal_gpio_init` @0x4202A828 与 E2 @0x4202476C 逐指令相同
  （仅 call/literal 地址不同）：BUSY=47、RST=48、CS=39、PWR=40；mask
  `0x8000/0x10000/0x80/0x100`；`gpio_set_level(48,1)(39,1)(40,1)`。
  SPI MOSI=41、SCLK=42 也在同一驱动里。
- **LUT**：720 B 表逐字节相同，MD5 `761cfaebe95df1d604480ca717df5f64`
  （E2S @0x3C17B4B0，E2 @0x3C17A1B4）。
- **充电检测**：GPIO5，mask `0x20`，`gpio_isr_handler_add(5)` 两边一致。
- **按键**：key.c 的 5 键字符串两边一致；同一个 GPIO 初始化函数里 GPIO7/8 的
  mask `0x80/0x100` 也一致。按键扫描脚的逐脚对比未做（没有发现 key.c 字符串差异）。
- **字体**：6 个 `lv_font_t` 结构都在，字段一致（12/16/18/22/30 px + montserrat_14）：
  - 12px：E2 0x3C16D738 / E2S 0x3C16EE18
  - 16px：E2 0x3C1737B4 / E2S 0x3C174CD0
  - 18px：E2 0x3C1762B0 / E2S 0x3C177550
  - 22px：E2 0x3C176468 / E2S 0x3C177708
  - 30px：E2 0x3C1766B0 / E2S 0x3C177950
  - montserrat_14：E2 0x3C1B8804 / E2S 0x3C1BE730
- **A5 设备 JSON**：除 `DevName` 外逐字段相同。
- **NVS**：27 键里 26 个相同，只差 `BLBri`。
- **音频/存储/无线**：ES8311（各 3 次）、I2S、SD/FATFS、Wi-Fi、Bluedroid、RTC
  的字符串/组件都在两边。

## 6. 未知

1. **E2 的 bootloader / 分区表 / flash 容量**：只有 app-only 包，无法验证。
   E2 app 能装进 E2S 的 0x1F0000 槽（余 33,488 B），但 E2 自己的槽位布局未知。
2. **没有 E2 真机**：所有结论都是静态的，未运行验证。
3. **E2 是否有物理耳机口**：固件没有检测代码，不能反推座子是否存在。
4. **CC1/CC2 的用途**：按代码看在 `headphone_check.c` 里，倾向耳机/附件检测；
   也可能是充电器 CC 检测，未实测。
5. **互刷行为**：未测试。E2 固件刷 E2S 预计能启动（屏/键/充电/音频对得上），
   但会丢背光和耳机/CC；E2S 固件刷 E2 会去初始化 AW2023 和耳机检测，
   如果 E2 的 I2C 35/18 或相关脚被别的功能占用，行为未知。

## 7. 复现

```sh
python3 tools/e2_e2s_diff.py
```

输出段表、哈希、关键字符串计数、源文件/GUI/NVS 差异、内嵌 Opus、LUT MD5、
字体结构与设备 JSON。

指令级复核（先把 IROM 段切出来）：

```sh
python3 - <<'PY'
import struct
for src, dst in [("downloads/paperang_e2-v1_3_759_13a9f.bin", "/tmp/e2_irom.bin"),
                 ("downloads/paperang_e2s-v1_3_739_e5938.bin", "/tmp/e2s_irom.bin")]:
    b = open(src, "rb").read()
    n, o = b[1], (24 if b[8] == 0xEE else 8)
    for _ in range(n):
        la, ln = struct.unpack_from("<II", b, o)
        o += 8
        if la == 0x42000020:
            open(dst, "wb").write(b[o:o+ln])
        o += ln
PY

OD=~/.espressif/tools/xtensa-esp-elf/esp-14.2.0_20260121/xtensa-esp-elf/bin/xtensa-esp32s3-elf-objdump
$OD -D -b binary -m xtensa --adjust-vma=0x42000020 \
    --start-address=0x4202A828 --stop-address=0x4202A8E8 /tmp/e2s_irom.bin
$OD -D -b binary -m xtensa --adjust-vma=0x42000020 \
    --start-address=0x4202476C --stop-address=0x4202482C /tmp/e2_irom.bin
```

## 8. 一句话给后续开发

- 从 E2S 固件删掉背光/耳机/CC 三块，基本就是 E2 固件的行为面；
- E2 的通用组件略新（i2c_bus、MD5 日志），E2S 的 media/产品层更全；
- 屏、键、充电、音频 codec、存储、无线在固件层没有发现差异；
- 没有 E2 真机和 E2 分区表之前，不要假设 E2 固件可以直接当 E2S 的降级包用。
