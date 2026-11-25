# E2S 8 MiB flash dump 记录

> 注：本文只保留分析记录。文中引用的 `dump/*.bin`、NVS 实际值、字体 TTF 与
> 原厂镜像均不随本仓库分发。

## 0. 结论

整片 dump 里确实有**反编译 app 拿不到**的东西：

- 出厂/运行时 NVS：SN、授权 key、绑定信息、电池/ADC 校准、MAC 校准、用户设置
- 精确的 stock build ID / ELF SHA-256、完整 bootloader、分区表、otadata 历史
- 分区表之外的 **3 个字体 blob**（Zpix + Alibaba PuHuiTi Light 两个子集），以及一个疑似字形位图缓存区
- PHY init 的 RF 校准数据

app 反编译只能得到代码和它引用的地址/格式，得不到这些**数据本身**。

## 1. dump 来源与可信度

| 文件 | 状态 | SHA-256 |
|---|---|---|
| `dump/e2s_flash_8mb_r5.bin` | 有效，写入新镜像**之前**的状态 | `dd078172d572d100826a5372e4d911cb5669b6316899f2f4f7bfff2f0a470a8d` |
| `dump/e2s_flash_8mb_r6.bin` | 有效，与 r5 逐字节相同 | 同上 |
| `dump/e2s_flash_8mb_r1.bin` / `r2.bin` | **无效**（ESP32 未复位，SPI 争用），勿用 | — |

- 读取条件：`CHIP_PU` 短地，ESP32-S3 停在复位；CH341A + flashrom `GD25Q64(B)`。
- 8,388,608 B；`0x0=0xE9`；`0x8000=AA 50`；分区表有效。
- r5/r6 是**写入新 badapple 之前**的完整备份。之后 `ota_0` 已被 `75b7b45c…` 覆盖，
  `ota_1`（原厂）、otadata、bootloader、NVS、字体区都没动。

复验：

```sh
python3 tools/e2s_flash_check.py dump/e2s_flash_8mb_r5.bin
sha256sum dump/e2s_flash_8mb_r5.bin dump/e2s_flash_8mb_r6.bin
```

## 2. 分区表（0x8000）

| 分区 | 类型 | 偏移 | 大小 |
|---|---|---|---|
| `nvs` | data:nvs | 0x009000 | 0x006000 |
| `otadata` | data:ota | 0x00F000 | 0x002000 |
| `phy_init` | data:phy | 0x011000 | 0x001000 |
| `ota_0` | app:ota_0 | 0x020000 | 0x1F0000 |
| `ota_1` | app:ota_1 | 0x210000 | 0x1F0000 |

- 8 MiB，bootloader header：SPI 模式 DIO、80 MHz、容量码 3（8 MiB）。
- 分区表 MD5 条目在 0x0080A0。
- `ota_1` 结束于 0x400000；之后直到 0x800000 **没有分区表条目**，但 0x480000 起有数据（见第 6 节）。

## 3. 原厂 app 身份（ota_1 @0x210000）

| 字段 | 值 |
|---|---|
| project | `paperang_e2` |
| version | `c6b2d574b-dirty` |
| built | `Oct 26 2023 09:54:47` |
| idf_ver | `c6b2d574b-dirty` |
| app ELF SHA-256 | `ebf8355f4faf9c9affc4f9ad68cc7c038566ab2a75f5e392dee175e7682b20ee` |
| 用户可见版本串 | `1.3.739` @ 0x213DCC |

这些是 OTA 包/反编译拿不全的：精确 build 时间、ELF 哈希、以及完整 bootloader 二进制
（0x0–0x8000）都在 dump 里。

## 4. otadata（0x00F000）

| 条目 | seq | state | CRC | 指向 |
|---|---|---|---|---|
| entry0 | 43 | 0xFFFFFFFF (undefined) | 有效 `0x88CCD7C0` | `(43-1)%2=0` → `ota_0` |
| entry1 | 42 | 0xFFFFFFFF (undefined) | 有效 `0x3070B0A5` | `(42-1)%2=1` → `ota_1` |

- 当前选 `ota_0`（seq 大者）。
- CRC 算法：`esp_rom_crc32_le(UINT32_MAX, &seq, 4)`，等价于
  `zlib.crc32(seq_le_bytes, 0xFFFFFFFF)`。
- seq=43 说明设备至少经历过 43 次 OTA/启动选择。

## 5. NVS（0x009000，0x6000）

已写入的 key（**值不记录**；含设备身份/授权信息）：

- 身份/授权：`sn` (15 B)、`key` (17 B)、`bind` (33 B)
- 出厂校准：`cal_data` (1904 B，2 chunk)、`cal_mac` (6 B)、`cal_version=502`
- 内容/状态：`bookCfg` (49 B)、`bookAvai`、`avai`、`ft` (25 B)、`ttf` (6 B)、
  `markIdx0/1`、`bookNew`、`hint0/1`、`helpMode`
- 用户/运行：`VoiceVol`、`VoicePlay`、`BLBri`、`nolearn`、`UsrRst`、`bleErr`、
  `RomLog`、`rstFlg`、`phy`、`user_info`

反编译 app 只能看到“它会读这些 key”，看不到这台设备上的实际值。尤其是
`sn`/`key`/`bind`/`cal_mac`/`cal_data`，不要外传。

## 6. 分区表外的字体区 @0x480000

这段数据**不在任何分区里**，stock app 直接按裸地址读。整个有效区
0x480000–0x528884（约 674 KB），后面直到 0x800000 全是 0xFF。

三个 FTF 容器（`0x40` 字节头 + 标准 TTF）：

| 字体 | FTF 偏移 | TTF 偏移 | TTF 大小 | 头 0x30 字段 | TTF SHA-256 |
|---|---|---|---|---|---|
| `Zpix-Regular` | 0x480000 | 0x480040 | 392,196 (0x5FC04) | 2190 | `5a83cc17…` |
| `Alibaba PuHuiTi-Light` | 0x4DFC44 | 0x4DFC84 | 81,984 (0x14040) | 560 | `806d0852…` |
| `Alibaba PuHuiTi-Light` | 0x4FB7A4 | 0x4FB7E4 | 146,108 (0x23ABC) | 880 | `e1b77728…` |

完整 SHA：

- Zpix：`5a83cc17efd1e926852953326beab32733fd11497e79ae619e8d02a83cdee9ba`
- PuHuiTi 子集 1：`806d08525a6535d5924eb084c3547e01c6c37db768f3024e0fab4bcaa30b8899`
- PuHuiTi 子集 2：`e1b77728603ca7752b28c46eceb03bca8541116273dc189322c8c1de348ebd1f`

FTF 头格式（小端）：

```
0x00  "FTTF"
0x04  字体名（NUL 结尾，0 填充）
0x24  u32 TTF 大小
0x28  0xFFFFFFFF
0x2C  0x00010001
0x30  u32（2190 / 560 / 880；不是 maxp 的 numGlyphs，含义待定）
0x34–0x3F  0
0x40  TTF 起始
```

`0x51F2A0` 之后、`0x520000` 起还有约 35 KB 非 0xFF 数据，无 FTF 头；字节分布是典型的
1bpp 图案（`0x00/0x40/0x35/0x80`），疑似字模/图标位图缓存，待进一步确认。

字体有版权（Zpix 是 OFL；Alibaba PuHuiTi 是商业字体），dump 和提取出的字体不要公开分发。

## 7. 反编译得不到的清单

- 设备身份：SN、授权 key、绑定信息、MAC 校准
- 出厂校准：`cal_data`（电池/ADC 等）、`cal_version=502`
- 精确 stock build / ELF SHA / 完整 bootloader / 分区表 / otadata 历史
- 分区表外 3 个字体 blob 和疑似字模缓存
- PHY init 的 RF 校准
- 用户设置和阅读状态

## 8. 当前设备状态

- `ota_0 @0x20000` 已写入新 badapple `75b7b45c…`（689,872 B），flashrom `VERIFIED`。
- `ota_1` 原厂未动；otadata 未动（仍选 `ota_0`）；NVS/字体区未动。
- 待验证：新固件启动后的 BLE 广播、`(1,241)` 诊断、`(3,2)` flash 信息。
