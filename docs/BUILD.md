# 构建与刷入

本文面向开发者和需要手动恢复的人。普通刷入只需要安卓端的 E2S 刷机助手，
步骤见 `README.md`。

## 目标硬件

| 项 | 值 |
|---|---|
| SoC | ESP32-S3，8 MiB flash，240 MHz |
| 面板 | 250×122 逻辑帧，1bpp，行 32 B，bit1=白 |
| 存储 | 内置 SD NAND，挂载到 `/sdcard`，没有卡槽 |
| 按键 | GPIO 2/4/6/7/8，低有效 |
| BLE | NimBLE，No_Security，不配对不加密 |
| OTA 槽 | `ota_0=0x20000`、`ota_1=0x210000`，各 `0x1F0000` |

## 构建

前置：ESP-IDF v6.1.1。默认路径 `~/esp/esp-idf-v6.1.1`，可用 `IDF_PATH` 覆盖。

```sh
cd firmware/e2_e2s_mp
./tools/build.sh                  # 等价 idf.py build
./tools/build.sh size             # idf.py size
./tools/build.sh fullclean build  # 显式 IDF 子命令
```

`tools/build.sh` 会逐参数拒绝所有写设备的 idf.py 子命令，包括 `flash`、
`app-flash`、`bootloader-flash`、`partition-table-flash`、`otadata-flash`、
`erase-flash`、`erase-otadata`、`read-otadata` 和 `dfu`。命中即退出 3。
它还会检查当前目录的工程名是 `e2s_mp`。

`main/CMakeLists.txt` 显式列出 8 个源文件，少一个会直接链接失败。构建配置
在 `sdkconfig.defaults`，关键项为 8 MB / DIO / 80 MHz、`-Os`、主任务栈
8192、FATFS 长文件名、NimBLE 无 NVS 持久化、`ATT_PREFERRED_MTU=247`。

2026-11-26 的构建结果：

```
Project build complete.
e2s_mp.bin binary size 0xe4b60 bytes (936,800 B).
Smallest app partition is 0x1f0000 bytes. 0x10b4a0 bytes (54%) free.
```

IDF 在构建末尾会打印 `To flash, run: idf.py flash` 和一条包含 bootloader、
分区表、otadata 的 esptool 命令。这是 IDF 的通用提示，不是本工程允许的
烧写方式。

## 无线刷入

使用安卓端的 E2S 刷机助手。助手会扫描广播名 `Paperang_E2S` 的设备，通过
原厂固件自带的 A5/OTA 服务把镜像写入非运行槽，再切换启动并验证版本。
不需要拆机或串口。助手工程在 `app/E2SFlash`，构建产物为
`app/E2SFlash/app/build/outputs/apk/debug/app-debug.apk`。

助手要求设备当前运行原厂固件。如果设备已经是 e2s_mp，先在设置中执行
“恢复原厂固件”，等设备重启回原厂后再刷入。

助手接受任意 E2S app-only `.bin`，但本地会校验 ESP app magic `0xE9`、
大小不超过 `0x1F0000` 和 SHA-256。刷入自定义镜像仍有风险；如果新固件在
BLE 启动前崩溃，只能用 UART 和 esptool 恢复。

## 手动刷写

e2s_mp 是 app-only 镜像。真机上只允许写两个地址：

| 允许 | 地址 | 大小 |
|---|---|---|
| `ota_0` | `0x20000` | `0x1F0000` |
| `ota_1` | `0x210000` | `0x1F0000` |

| 绝对不要写 | 内容 |
|---|---|
| `0x000000` | bootloader |
| `0x008000` | 分区表 |
| `0x009000` | NVS |
| `0x00F000` | otadata |
| `0x011000` | phy_init |
| `0x010000` | otadata 内部，不是 app 起始地址 |
| `0x200000` | ota_0 尾部，不是 ota_1 起点 |
| `0x480000` | 原厂字库区（`0x480000..0x528884`） |

安全流程：

1. 只读回读 otadata：
   `python -m esptool ... read-flash 0xf000 0x2000 /tmp/otadata.bin`。
2. 按 ESP-IDF bootloader 规则解析两个 entry，确定活动槽。有效项取 seq
   最大者，两个都无效时按 IDF 默认槽。
3. 只写活动槽。写非活动槽不会改变启动选择，还会覆盖该槽里可能存在的
   原厂恢复镜像。活动槽是原厂镜像时，写入 e2s_mp 会覆盖它；只有另一槽
   仍保留有效原厂镜像时，固件内的“恢复原厂”才有目标。
4. 写入：
   `python -m esptool ... write-flash <活动槽地址> build/e2s_mp.bin`。
5. 回读同一地址并比对 `sha256sum`。
6. 写失败时重新进入 ROM 下载模式，不要改 otadata 来修。

`tools/flash_app.sh` 只打印命令，不打开串口，也不调用 esptool：

- 默认镜像 `build/e2s_mp.bin`，超过 `0x1F0000` 退出 3。
- `--otadata /tmp/otadata.bin` 解析本地 dump，按 CRC32、seq 和
  `(seq-1)%2` 选择活动槽，只打印该槽的写命令。
- `--slot ota_0|ota_1` 只给已经人工确认活动槽的人用。
- 两个参数都不给时，只打印 otadata 只读回读命令和下一步提示。

永远不要运行 `idf.py flash`。它会按 `build/flash_args` 一次写 bootloader、
分区表、`ota_data_initial.bin` 和 app。`partitions.csv` 与真机表一致，
不代表这条命令安全。

## README 预览图

README 的界面总览图由模拟器输出合成：

```sh
./sim/run.sh --all
python3 tools/make_readme_sheet.py
```

脚本从 `sim/out/order.txt` 中取选定的 12 帧和对应的 `_1x.png`，生成
`docs/images/screens.png`。需要系统安装 Noto Sans CJK SC。

锁屏示例图的源文件是 `tools/assets/lock_demo_src.png`，由
`tools/gen_lock_demo.py` 先把透明背景合成到白底，再转成 `sim/lock_demo.h`
中的 4000 B 1bpp 图像，底部加上“图片锁屏示例”。

## 验证

| 检查 | 命令 | 结果 |
|---|---|---|
| ESP-IDF 构建 | `./tools/build.sh build` | `Project build complete`；936,800 B（0xe4b60），槽剩余 54% |
| 模拟器断言、哈希、文案 | `./sim/run.sh --check` | 25 帧，0 失败；289 条文案通过 |
| 模拟器出图 | `./sim/run.sh --all` | 25 PNG 和 `contact_sheet.png` |
| 占位 UI 反例 | `./sim/run.sh --check --ui dummy --hints dummy` | 大面积失败，检查不是空跑 |
| 文案 lint | `sh tools/check_copy.sh` | 289 literals OK |
| 烧录脚本 | `./tools/flash_app.sh` | 只打印 otadata 只读回读命令 |
| 构建脚本拒绝烧写 | `./tools/build.sh flash` | `refusing 'flash'`，退出 3 |

本轮没有运行：

- 真机连接、EPD 面板、SD 挂载、按键实测、电量与充电。
- 原版 MiaoPaper.html 的 BLE 端到端、1 MB 上传、MTU 协商、
  `0x33` 回读、`0xDD` 跨日走时。
- 恢复原厂的 probe 和 commit、`esp_restart()`、失败分支。
- 掉电、半写、坏卡注入、长时间 GC 和残影、电流测量。
- `idf.py flash` 或 esptool 写设备，按设计不运行。

## 目录

```
e2s_mp/
├── main/                  # 8 个源文件 + 公共头 + fonts/
│   ├── mp_app.c           # 状态机、按键、BLE 分发、刷新、恢复
│   ├── mp_ui.c/.h         # 250×122 渲染 + 视图模型
│   ├── mp_page.c/.h       # 分页，与 Miaopaper 对齐
│   ├── mp_store.c/.h      # /sdcard/mp 的目录、书、锁图、进度、统计
│   ├── mp_ble.c/.h        # NimBLE GATT、广播、队列、通知
│   ├── mp_hints.c/.h      # 底栏键帽与逐屏提示
│   ├── mp_font.c/.h       # Adafruit GFX 字体适配
│   └── main.c             # 启动顺序与初始化
├── docs/                  # BUILD / DESIGN / KEYMAP / PROTOCOL / COPY_DECK / PORT_SPEC
├── sim/                   # 主机模拟器
├── tools/                 # build.sh / flash_app.sh / check_copy.sh / gen_lock_demo.py / make_readme_sheet.py
├── partitions.csv
└── sdkconfig.defaults
```
