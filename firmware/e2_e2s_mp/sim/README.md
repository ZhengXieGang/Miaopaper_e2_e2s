# sim: 主机模拟器

`sim/` 不用 ESP32、ESP-IDF 或 SDL。它用 gcc 编译真实的 `main/mp_ui.c`、
`main/mp_hints.c`、`main/mp_font.c`、`main/mp_page.c`，加上
`../e2s_reader/components/display/gfx.c`、`components/text/{text_font,
text_encode,font8x16}.c` 和 `components/common/crc32.c`，渲染 250×122
1bpp 帧，写成 PGM，再由 Pillow 拼成 PNG 和 contact sheet。

它验证渲染层，不验证固件。`mp_app.c`、`mp_store.c`、`mp_ble.c` 和
`main.c` 不参与编译。`mp_page.c` 参与编译，READ 三帧由真实分页器生成。

## 快速开始

```sh
cd firmware/e2s_mp

sim/run.sh --all                    # 编译 + 25 帧 + contact_sheet.png
sim/run.sh --one clock              # 单帧
sim/run.sh --check                  # 编译 + 版面断言 + 25 帧哈希 + 文案 lint
sim/run.sh --all --out /tmp/mp_sim  # 指定输出目录
sim/run.sh --all --no-build         # 复用 sim/build/sim_mp
sim/run.sh --all --ui dummy --hints dummy   # 占位渲染器，只查管线
```

依赖 `gcc`（C11）、`make`、`python3` 和 Pillow。Pillow 只用于 PNG，PGM
不需要。字体资产默认取
`../../e2s_reader/components/text/assets/{HZK16_87.bin,gb2312_map.bin}`。
编译期可用 `make -C sim TEXT_ASSETS=/path` 覆盖，运行期可用
`MP_SIM_ASSETS=/path sim/run.sh ...` 覆盖。资产缺失不致命：每个 CJK 字符
按 text 组件的行为画 16 px 空心框，stderr 会打印缺的是哪份。

## 真实 UI 还是占位 UI

`sim/Makefile` 自动检测：

| `main/mp_ui.c` 和 `mp_hints.c` | 编译进模拟器的实现 |
|---|---|
| 存在 | 真实文件，`UI=real` 或 `HINTS=real` |
| 不存在 | `sim/stub_ui.c` 或 `sim/stub_hints.c`，示意图，不是出货 UI |

仓库里的 `main/mp_ui.c` 和 `main/mp_hints.c` 已经落地，所以默认构建就是
真实 UI，`--check` 也对着真实渲染结果运行。强制选择用
`make -C sim UI=real`、`make -C sim UI=dummy`，`HINTS=` 同理，或
`sim/run.sh --ui dummy --hints dummy`。`make -C sim print-config`
打印当前选择。

## 输出

`--out DIR` 默认是 `sim/out`：

| 路径 | 内容 |
|---|---|
| `pgm/NAME.pgm` | 原始帧，P5 250×122，255 表示白，0 表示黑 |
| `NAME.png` | 3× 最近邻放大，带标题条 |
| `NAME_1x.png` | 250×122 原始像素 |
| `contact_sheet.png` | 2 列网格，3×，标题条，顺序同 `order.txt` |
| `order.txt` | 帧顺序 |

帧共 27 个：

```
clock select read_cn read_en read_end settings settings_off
settings_scroll settings_e2 backlight backlight_popup book_info jump
time_set time_done delete about about_e2 lock_img lock_stats lock_none
no_lock_img empty error low_batt restore restore_ok
```

- `read_cn`、`read_en`、`read_end` 由真实 `mp_page_measure()` 从连排 3 遍的
  fixture 书生成，`read_end` 是真正的末页，只有 2 行。
- `settings_off` 覆盖蓝牙关闭，`settings_e2` / `about_e2` 覆盖无背光（E2）自适应视图。
- `lock_img` 是纯锁屏图，`lock_stats` 是阅读统计，`lock_none` 是解锁提示页，
  `no_lock_img` 是未上传锁屏图的提示页。
- `backlight` 是背光设置页，`backlight_popup` 是阅读上的快捷背光浮窗，
  `low_batt` 是电量耗尽关机提示页。

### 极性

帧 bit 1 表示白（`reader_cfg.h`），PGM 写 255；bit 0 表示黑，PGM 写 0。
`gfx_clear(fb, 0)` 得到白底，参数叫 black，0 表示不画黑。渲染后不要反转。

### 锁图

`lock_img` fixture 由 `tools/assets/lock_demo_src.png` 经
`tools/gen_lock_demo.py` 生成（透明背景先合成到白底），底部有
“图片锁屏示例”文字。源是 4000 B
`epd_temp` 布局：`idx=(y>>3)*250+x`，`bit(y&7)`，
1 为黑，共 128 行。屏幕只显示源 `y=6..127`，映射到帧 `y=0..121`。
顶部 6 行在设备上不可见，这是 web 编辑器按 250×128 构图、设备裁切显示的
结果，不是 bug。`sim_main --check` 会逐像素比对这条映射，包括极性。

## `--check` 验证什么

- 每帧墨点数在 [200, 30300] 内，既非全白也非全黑。
- 每行 x=250..255 的填充位保持白，渲染越界会踩到。
- 非 LOCK 屏除 CLOCK 外，y=18 必须是整行黑线，y=19 全白。
  CLOCK 的 y=18 必须全白。解锁提示页的 y=0..18 必须完全空白。
- `lock_img` 整帧与 `epd_temp[y+6]` 逐像素一致，检查极性和 6 行偏移。
- READ 三帧的 y=110..117 和 y=120..121 必须空白。`(2,119)` 和
  `(247,119)` 是 2 px 进度条的底线，`(2,118)` 是已读部分。
- SELECT 和 SETTINGS 的微圆角反色条、BOOK_INFO 的 2×2 按钮选中态、
  TIME_SET 的字段反色框都存在。
- 25 帧 CRC32 与 `expected.h` 一致。
- `sim/run.sh --check` 还会调用 `tools/check_copy.sh`，检查禁止词、
  英文键名、“插卡”措辞和宽度。

2026-11-25 的实际运行：

```
$ ./sim/run.sh --check
...
PASS lock_stats      ink=1920
PASS lock_none       ink=744
...
check: 25 frames, 0 failure(s)
copy: 289 literals OK (9 files)
# 退出码 0

$ ./sim/run.sh --all
sheet: .../sim/out/contact_sheet.png (25 frames, 2 columns)
# 退出码 0
```

反例：`./sim/run.sh --check --ui dummy --hints dummy` 会大面积失败，
几何断言和 `expected.h` 哈希都对不上。这说明 `--check` 不是空跑。

## 字体与私有头

`text_assets_set_host()` 只在 `-DTEXT_HOST_BUILD` 下声明于私有头
`components/text/text_internal.h`。`sim/sim_text.c` 是 `sim/` 里唯一读资产
文件的文件。`HZK16_87.bin` 优先，缺失时回退 `hz16_gb2312.bin`，两者逐字节
相同，都是 261,696 B。`gb2312_map.bin` 为 29,792 B，由 `text_font.c`
校验 magic、长度和 CRC。

UI 的 ASCII 与大号时钟使用 `main/fonts/font_ui16.h` 和 `font_clock.h`。
这两份字模由 `tools/gen_ui_font.py` 从 Inter 生成，许可为 SIL OFL 1.1，
不读系统字体。

## 已知限制

1. `mp_page.c` 只走正向分页。READ 三帧由真实 `mp_page_measure()` 从连排
   3 遍的书生成。上一页和历史栈不在本 sim 内。
2. `expected.h` 是像素真源。25 帧各有一个 CRC32，由 `sim --hash` 生成。
   UI 改动后必须显式重新生成，否则失败。
3. `SIM_UI_KIND` 不是显式失败条件。它只打印 `sim_ui: real` 或 `dummy`，
   但 `expected.h` 的哈希只匹配真实 UI。
4. 阅读正文基线为 `READER_ASCII_DY=3`。UI 的 ASCII 用 Inter Regular 16，
   基线为 top+15；中文 HZK16 顶等于 top。两套规则不要混用。
5. 不验证固件行为。BLE、上传、存储、按键状态机、刷新、GC 和恢复原厂都
   不在本构建内，只能上真机或用 app 层夹具。
6. Pillow 的 `ImageFont.load_default(size=...)` 需要 Pillow 10.1 以上。
   更老的版本会回退到无参默认字体，标题变小，但生成仍成功。
7. 路径含空格用引号处理。超长路径会报错，不会静默写到错误位置。

## 文件

| 文件 | 作用 |
|---|---|
| `Makefile` | gcc 编译真实 UI、字体、gfx、mp_page 和 sim 源，自动选择 real 或 dummy |
| `sim_main.c` | 25 个视图模型经 `mp_ui_render()` 输出 PGM，并跑断言 |
| `expected.h` | 25 帧 CRC32，由 `--hash` 生成 |
| `sim_text.c/.h` | 读两份字体资产并调用 `text_assets_set_host()` |
| `sim_store.c/.h` | 内存 fixture：3 本书、8 行设置、ABOUT、锁图、阅读行 |
| `lock_demo.h` | 真实锁屏示例图的 4000 B 1bpp 数据，由 `tools/gen_lock_demo.py` 生成 |
| `stub_ui.c` | 占位 `mp_ui_*`，用于 `UI=dummy` 或缺少真实文件时 |
| `stub_hints.c` | 占位 `mp_hints_*`，用于 `HINTS=dummy` 或缺少真实文件时 |
| `make_contact.py` | PGM 转 3× PNG 和 2 列 contact sheet，依赖 Pillow |
| `run.sh` | 构建、渲染、出图；支持 `--all/--one/--check/--out/--ui/--hints/--no-build` |

## 全链路固件回归测试（`sim_fw_regress`）

除静态 30 帧 UI 渲染与像素黄金哈希回归外，`sim/` 现已内置**本机全链路固件行为回归测试套件**（`sim_fw_regress.c` + `sim_hal.c` + `host_inc/`），直接在 Linux 主机上编译真实生产核心源码：

- `main/mp_app.c`、`main/mp_store.c`、`main/mp_toc.c`、`main/mp_ota.c`
- `main/mp_ui.c`、`main/mp_hints.c`、`main/mp_font.c`、`main/mp_page.c`
- `components/recovery/recovery.c`、`components/display/gfx.c`、`components/text/*`、`components/common/*`

特点：
- 默认开启 `-Wall -Wextra -Werror -fsanitize=address,undefined`（ASan + UBSan 内存越界与未定义行为实时检测）；
- 每个用例运行在独立 `fork()` 子进程与独立 `/tmp/mp_fw_reg_*` 虚拟 SD 沙盒内（退出自动清理）；
- 内置 8 MiB 虚拟 SPI Flash（含硬件级写保护红线、NOR 按位与物理特性、FIPS 180-4 SHA-256 与 ESP32-S3 镜像格式验证器），`T11` 会优先自动加载真实构建产物 `build/miaopaper_e2_e2s.bin` 跑完 `0x50..0x53` BLE OTA 与 IRAM 覆写审计；
- 通过 ELF 自定义段 `mp_sim_rtc` 复刻 ESP32-S3 `RTC_DATA_ATTR` 慢速内存，支持双进程冷启动真模拟跨深睡唤醒与走时保持。

常用命令：

```sh
cd firmware/e2_e2s_mp

sh sim/run.sh --check             # 一站式：30 帧 UI 校验 + 文案 lint + 17 项固件全链路回归
sh sim/run.sh --regress           # 仅运行 17 项本机固件回归测试
sh sim/run.sh --regress --only T11 # 仅运行单项（如真实镜像原地 BLE OTA 测试）
make -C sim regress SANITIZE=0    # 关闭 Sanitizer 快速编译运行
```

## 排错

- `sim_assets: ... missing`：CJK 会变空心框。检查 `MP_SIM_ASSETS` 或
  `make -C sim TEXT_ASSETS=...`。
- `make: *** ... mp_ui.c does not exist`：用了 `UI=real` 但文件不存在。
  去掉 `UI=real` 或改用 `UI=dummy`。
- `make_contact.py: Pillow is not installed`：运行 `pip install Pillow`。
  PGM 已经生成，也可以用其他工具查看。
- `--check` 在 `--ui dummy` 下大面积失败是预期行为。占位帧不满足真实版的
  留白、反色和进度条规则，也匹配不上 `expected.h`。
