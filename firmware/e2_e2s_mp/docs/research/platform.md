# e2s_mp 平台与硬件盘点(只读研究)

范围:`firmware/e2s_mp`(现有 `mp_store/mp_font`、字体表、CMake、sdkconfig、partitions)、
`firmware/e2s_reader/components`(display/input/power/storage/text/recovery/ble)、
`docs/E2S_READER_INVENTORY.md`、`components/common/include/board.h`,以及
`refs/Miaopaper/Firmware/src`(行为参考)与 `firmware/e2s_re`(原厂路径/证据)。

本文件只记录**读到的代码**与**本次实际跑过的检查**。没有上真机、没有刷机、没有挂载 SD、
没有提交 recovery、没有跑 BLE;凡属这类都标“未运行”。

---

## ResearchBrief

1. **e2s_mp 现在只差一个 `main` 组件就能链接**:所有 9 个复用组件在 IDF v6.1.1 下都能编译,
   唯一致命错误是 `undefined reference to 'app_main'`;补 `main/CMakeLists.txt` + 一个
   `app_main` 后 `idf.py build` 成功,镜像 **537,792 B**(槽上限 0x1F0000)。
2. **`mp_font.c` 的比例字体渲染是坏的**:`mp_font.c:53-55` 用 `gfx_blit1(...,(w+7)/8)` 按
   “每行字节对齐”读字形,而 `fonts/font16.h` 的位图是**位连续打包**(无行填充,见
   `font16.h:101-106` 的 `bitmapOffset`)。已用同一份代码左右对比渲染证明:当前实现 ASCII
   重叠成一团,改成位连续后正常。中文 HZK 走 2 B/行,不受影响。
3. **`mp_store` 的写路径缺“声明长度”校验**:`mp_store_write()` 接受任意 `off/len`,
   `mp_store_finish()` 不核对文件长度就置 `DONE`(`mp_store.c:363-395`)。被截断的上传会
   在 0x19/书目里显示为完成,直到打开书才读失败。
4. **catalog 不是原子的,且“读失败即重建”会丢数据**:`flush_catalog_locked()` 先
   `unlink` 再 `open`(`mp_store.c:160-161`);`load_catalog_locked()` 对存在但短读/魔数错的
   catalog 直接 `memset 0xFF` 并覆盖写(`mp_store.c:182-209`),没有备份/改名恢复。
5. **`/sdcard/mp` 与 storage 组件的 FS 句柄没有协调**:`storage_remount()` 会卸载卷
   (`storage_sd.c:305-363`),但 `mp_store` 的 `s_wfd` 不会被关;两边各用各的互斥量,
   IDF FAT VFS 只在**单次** `open/read/write` 上加卷锁(`vfs_fat.c:394` 起),多步序列不原子。
6. **按键没有双击**:`input` 只产生 DOWN/CLICK/LONG/REPEAT(`input.h:20-25`),
   `READER_KEY_LONG_MS=1000` **确实生效**(本次构建的 `compile_commands.json` 里
   `input_keys.c` 带 `-DREADER_KEY_LONG_MS=1000`)。要复刻 Miaopaper 的 F 双击=上一页,
   只能在 app 层用两次 CLICK 的时间差合成,代价是单击要人为延迟 ~350 ms。
7. **内置 SD NAND 证据充分**:原厂用 SDMMC 4-bit + GPIO34 供电门控(INVENTORY §1/§3.4),
   书/字库/音频/**OTA 镜像**都在 `/sdcard` 下(`e2s_re/.../paperang_ble.c:43-44,352-357`),
   `hello_paperang/README.md:104` 明写“外部 SD NAND”,且 8 MiB dump 里**没有**任何
   “插卡/no card”字符串。UI 必须写“存储不可用/存储错误”,不能写“请插卡”。
8. **显示优势可以完整保留**:`display` 组件已有“计数 GC + 45 s 空闲 GC + 全刷最小间隔 5 s +
   GC 后 5 s 降级”(`display.c:79-189`),比 Miaopaper 的纯计数 GC(`epd_refresh.c:31-67`)更好。
   但 `DISPLAY_MODE_PARTIAL` 并不强制局刷、`DISPLAY_MODE_FULL` 在边界可能被降级,只有
   `display_request_full()` 是“不可降级但会等 5 s”。
9. **深睡不可用**:`power_enter_deep_sleep()` 永远返回 `ESP_ERR_NOT_SUPPORTED`
   (`power_battery.c:293-301`),`epd_deep_sleep()` 是空操作(`epd.c:583-586`),
   sdkconfig 也没有 `CONFIG_PM_ENABLE`。mp 的 15/30/60/120 s 睡眠只能降级成
   “锁屏/待机画面 + 停广播 + 停 SD”,不能调用 `esp_deep_sleep_start()`。
10. **主机模拟可行但有两个硬点**:`gfx.c`/`text_font.c`/`mp_font.c` 已用 gcc 16.2.1 +
    `-DTEXT_HOST_BUILD` 编译通过(Pillow 12.3.0 / ImageMagick 7.1.2 已用于出 PNG);
    `mp_ui.c` **不存在**,无法验证;`mp_store.c` 能靠 3 个桩头文件编过,但 `MP_DIR` 写死
    `/sdcard/mp`,主机上 `mp_store_init()` 直接失败(`mkdir /sdcard/mp failed`,已实测)。

---

## 0. 方法与已执行的检查

### 0.1 读过的文件

- `firmware/e2s_mp/`:CMakeLists.txt、sdkconfig.defaults、partitions.csv、main/mp_store.{h,c}、
  main/mp_font.{h,c}、main/fonts/font16.h、font16zh.h、font30.h、font_60.h。
- `firmware/e2s_reader/components/`:common/{board.h,reader_cfg.h,rc_common.h}、
  display/{display.c,include/display.h,gfx.c,include/gfx.h,display_internal.h}、
  e2s_epd/{epd.c,epd.h}、input/{input_keys.c,include/input.h}、power/*、storage/{storage_sd.c,include/storage.h}、
  text/{text_font.c,include/text.h,text_internal.h,CMakeLists.txt,assets/NOTICE.md}、
  recovery/*、ble/{include/reader_ble.h,ble_proto.h}、各组件 CMakeLists.txt。
- `firmware/e2s_reader/{CMakeLists.txt,main/CMakeLists.txt,main/reader_app.c,main/reader_ui.c,
  docs/{DESIGN.md,USAGE.md},tools/{build.sh,flash_app.sh,run_host_tests.sh}}`。
- `docs/E2S_READER_INVENTORY.md`;
- `refs/Miaopaper/Firmware/src/{ebook.h,ebook.c,ebook_buttons.c,ebook_ble.c,epd_refresh.c,flash.c}`;
- `firmware/e2s_re/main/{main.c,sdcard.c}`、`components/ble/paperang_ble.c`、
  `components/word_manager/*`、`components/paudio/player.c`、`components/freetype2/diskio.c`。

### 0.2 实际跑过的检查(命令 + 结果)

| # | 命令 | 结果 |
|---|---|---|
| 1 | `cp -a e2s_mp /tmp/mpbuild/` + 软链 `e2s_reader/components`,在 `/tmp/mpbuild/e2s_mp` 执行 `idf.py reconfigure && idf.py build`(IDF v6.1.1) | 所有组件编译通过;链接失败:`undefined reference to 'app_main'`(`freertos/app_startup.c:204`)。说明 `main` 组件不存在(IDF 不会报“缺 main/CMakeLists.txt”,而是在链接期暴露) |
| 2 | 在 /tmp 副本中补 `main/CMakeLists.txt`(`SRCS mp_store.c mp_font.c main_stub.c`, `REQUIRES common text display storage input power`)与一个调用 `mp_store_init`/`mp_text_draw`/`display_post` 的 `app_main` 桩,再 `idf.py reconfigure && idf.py build` | `Project build complete`;`build/e2s_mp.bin` = **537,792 B**;`grep -c warning: build2.log` = **0**;`mp_store.c`/`mp_font.c` 均参与编译 |
| 3 | `BUILD_DIR=/tmp/mp_host_tests CC=gcc sh tools/run_host_tests.sh`(e2s_reader 现有脚本) | `985765 checks, 0 failure(s)`;`recovery: 92 checks, 0 failure(s)` |
| 4 | 主机构建 `gcc -std=c11 -O2 -Wall -Wextra -Werror -DTEXT_HOST_BUILD -include stdbool.h` 编译 `mp_font.c` + `gfx.c` + `text_font.c` + `font8x16.c` + `crc32.c` | 退出码 0,无告警 |
| 5 | 主机渲染桩:读 `assets/hz16_gb2312.bin` + `gb2312_map.bin` → `text_assets_set_host()` → 画 ASCII/GB2312 → 写 PGM → `magick frame.pgm -scale 300% frame.png` | 出图成功;ASCII 明显重叠乱码 |
| 6 | 左右对比:同一字符串,左用 `mp_gfx_text()`(现有实现),右用“位连续”blit(自写) | 右半正常、左半乱码;证明问题在 blit 步长而非字体数据 |
| 7 | 编译 `mp_store.c` 到主机(自制 `esp_log.h`/`freertos/FreeRTOS.h`/`freertos/semphr.h` 三个桩头,`-D_GNU_SOURCE`) | 编译通过 |
| 8 | 链接并运行 `mp_store_init()` 主机探针 | `rc=-1 err=mkdir /sdcard/mp failed`(路径写死,主机无 `/sdcard`) |
| 9 | `diff -u e2s_reader/partitions.csv e2s_mp/partitions.csv` | 无差异(rc=0) |
| 10 | 在 /tmp 构建目录的 `compile_commands.json` 里查 `input_keys.c` / `mp_store.c` | 两条命令都含 `-DREADER_KEY_LONG_MS=1000` |
| 11 | `grep -rn "esp_partition_write\|esp_partition_erase\|esp_ota_write\|esp_flash_write\|esp_flash_erase" components/` 与 `grep -rn "nvs_" components/` | 写 flash 的只有 `recovery/recovery.c:358,364`;组件里没有任何 `nvs_*` 调用 |
| 12 | `strings -a dump/e2s_flash_8mb_r5.bin \| grep -iE "sd card\|no card\|insert\|请插\|TF card"` | SD 相关的只有 ESP-IDF 自带的 “Make sure SD card lines have pull-up resistors” 与 `========> FATAL: SD card error <========`;没有 “no card / insert card / 请插卡 / TF card”;其余 `insert` 命中是 `headphone_insert_detect`、TCP `insert` 等无关串 |
| 13 | 环境:`gcc --version` / `python3 -I -c "import PIL"` / `magick -version` / `ls ~/esp` | gcc 16.2.1;PIL 12.3.0;ImageMagick 7.1.2-32;存在 `~/esp/esp-idf-v6.1.1` |

### 0.3 未运行

- 真机启动、EPD 初始化、SD 挂载、按键实测、BLE 连接、recovery 提交、深睡唤醒、电流测量。
- 没有在仓库内的 `build/` 下构建(所有构建都在 `/tmp` 副本,仓库未被写入;本文件是唯一新增文件)。

---

## 1. 构建缺口:现在缺什么

### 1.1 缺口清单

| 缺什么 | 证据 | 后果 |
|---|---|---|
| `main/CMakeLists.txt` | 目录里只有 `mp_store.c/h`、`mp_font.c/h`、`fonts/`(`ls -la main`) | `mp_store`/`mp_font` 根本不参与编译;链接期 `undefined reference to 'app_main'` |
| `main/main.c`(或任意 `app_main`) | `grep -rn "app_main\|mp_ui" e2s_mp` 无命中 | 同上 |
| app 层(`mp_app.c`/`mp_ui.c`/`mp_ble.c`) | 不存在 `mp_ui.c`;目录里只有两个模块 | 无法显示、无法收 BLE、无法处理按键 |
| `tools/build.sh`、`tools/flash_app.sh`、host 测试 | `find e2s_mp/tools e2s_mp/docs -type f` = 0 | 没有安全构建/烧写入口;需从 `e2s_reader/tools/` 复制并改镜像名 |
| `docs/` | 空目录 | 本文件落在 `docs/research/` |

### 1.2 最小可构建集合(已实测)

`main/CMakeLists.txt` 至少:

```cmake
idf_component_register(SRCS "mp_store.c" "mp_font.c" <app 源文件...>
                       INCLUDE_DIRS "."
                       REQUIRES common text display storage input power)
```

- 用到 BLE 时加 `ble`;用到恢复原厂时加 `recovery`。
- `mp_store.c` 需要 `text`(调用 `text_hz16_glyph()`,`mp_store.c:21,230,561`);
  `mp_font.c` 需要 `display`(`gfx_blit1/gfx_frame`)与 `text`。
- 组件自身依赖(已验证在 v6.1.1 下可解析):
  - `display`: `REQUIRES common`,`PRIV_REQUIRES e2s_epd esp_driver_gpio esp_timer`
  - `storage`: `PRIV_REQUIRES fatfs esp_driver_sdmmc sdmmc esp_driver_gpio vfs`
  - `text`: `REQUIRES common display` + `EMBED_FILES` 两份资产
  - `input`: `REQUIRES common`,`PRIV_REQUIRES esp_driver_gpio esp_timer`
  - `power`: `REQUIRES common`,`PRIV_REQUIRES esp_adc esp_driver_gpio esp_timer`
  - `recovery`: `PRIV_REQUIRES app_update bootloader_support efuse esp_app_format esp_hw_support spi_flash`
  - `ble`: `REQUIRES common storage power`,`PRIV_REQUIRES bt`
- 顶层 `CMakeLists.txt:9` 的 `EXTRA_COMPONENT_DIRS ../e2s_reader/components` 在 `/tmp` 软链下也工作;
  `CMakeLists.txt:13` 的 `add_compile_definitions(READER_KEY_LONG_MS=1000)` 会传播到组件
  (已用 `compile_commands.json` 证实)。

### 1.3 复用 ../e2s_reader/components 的边界

- 共享组件的**公共头是冻结的**(各头文件首行注释),e2s_mp 不应改它们;需要 mp 专属行为时
  写在自己的 `main/` 里,或新增组件(不要改 `input/display/storage`)。
- `text` 组件的两份资产是**编译期内嵌**(`text/CMakeLists.txt:19-43`),所以 e2s_mp 的镜像
  会自动带上 261,696 B 的 HZK16 与 29,792 B 的 map;`mp_store` 只是“存一份上传字库”,
  渲染仍用内嵌字库(`mp_store.h:11-12`)。
- `recovery` 在 `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE`/`ANTI_ROLLBACK` 打开时会 `#error`
  (`recovery.c:38-40`);本次 sdkconfig 是 `# CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE is not set`。

---

## 2. mp_store:与参考实现的对照、接口与边界问题

### 2.1 格式对照(逐项核对 `refs/Miaopaper/Firmware/src/ebook.h`)

| 对象 | Miaopaper | mp_store | 结论 |
|---|---|---|---|
| catalog 头 | `"BOOK"` + ver 1 + `[5]=font_inst` + `[6]=book_cnt(未用)` + `[7]`(`ebook.h:29-33`;SURVEY `:336`) | 8 B,只写 `[0..4]` 与 `[5]`(`mp_store.c:196-203,177-180`) | 一致;`[6]` 不更新也无妨(参考实现也不依赖) |
| 书条目 | 32 B:flags@0、title@1(19 B + NUL)、start@0x15、len@0x19、enc@0x1D(`ebook.h:35-42`) | 完全相同(`mp_store.c:137-152`) | 一致;`start` 恒 0 合理(SD 上一书一文件) |
| 标志 | EMPTY 0xFF / UPLOADING 0xFE / DONE 0xFC / DELETED 0x00(`ebook.h:46-52`) | 同(`mp_store.h:26-29`) | 一致 |
| 锁图 | 4000 B,有效标志 `0xAA` 在**同一 4 KiB 扇区**的 `+0x0FF0`(=4080)(`ebook.h:14-17`) | 4000 B + flag@0x0FF0;`lock_begin` 先写 offset 4095 把文件撑到 4096(`mp_store.c:35-37,495-508`) | 语义一致;头注释“4000 B”容易让人误以为 flag 在 4000 以内 |
| 进度 | **只追加环**:512 条 8 B 记录,读时扫最新(`ebook.h:18-28`) | **定长 8 槽**,每本固定 `idx*8`(`mp_store.c:38-39,621-662`) | 记录格式相同(`0x5A,idx,pos[4],xor`),容器不同;仅为内部文件,可接受,但必须写进文档 |
| 设置 | 完整 `settings_struct`,4 个 ebook 字段 + 其它 | 只存 4 个 ebook 字段(`mp_store.h:44-49`) | 子集;0x34/0x35 只用到 `ble_enabled`/`sleep_timeout_idx`(`ebook_ble.c:594-596`) |
| 字库 | 261,696 B 裸字模 + 目录 `hdr[5]` 标志(`ebook.h:6,32`;SURVEY `:254-256`) | `FONT_BYTES 261696`、`font.bin`、`font_finish` 置 flag(`mp_store.c:38,567-608`) | 尺寸一致 |
| 睡眠/GC 选项 | `{15,30,60,120}`、`{10,20,50,100}`,`sleep_idx=2`,`partial=0`,`gc_idx=0`(`flash.c:13-15,50-52`) | 同数组、同默认(`mp_store.c:45-47,675-681`) | 一致 |

### 2.2 必须修的问题(按严重度)

#### P0-1 上传长度无校验 → 截断的书被标成完成

`mp_store_write()` 只检查 `buf` 与 `s_wfd/s_wslot`(`mp_store.c:363-377`),不检查
`off + len <= 已声明 len`;`mp_store_finish()` 只 `fsync/close` 后置 `DONE`
(`mp_store.c:379-395`),**从不 `stat()` 比对声明长度**。
后果:BLE 0x12 提前到达、丢包、主机算错长度,都会得到一本“DONE 但短”的书;
`mp_store_book_size()` 返回声明 `len`,读到文件尾才发现短读。

要求:
- `begin` 记录 `declared_len`;`write` 拒绝 `off > declared`、`len > declared - off`、
  `off + len` 32 位溢出;
- `finish` 必须 `stat()` 且 `st_size == declared_len`,否则返回错误并置回 `EMPTY`
  (或 `UPLOADING`),不得置 `DONE`;
- 同时记录“已写最大偏移”,拒绝空洞(参考实现 0x11 回 `written` 就是让主机发现 gap,
  `ebook_ble.c:241-280`)。

#### P0-2 读 catalog 失败会**覆盖**现场

`load_catalog_locked()` 在 `open` 失败、`read_all` 短读、魔数/版本不符时,
直接 `memset(entries, 0xFF)` 并 `flush_catalog_locked()`(`mp_store.c:182-209`)。
一张被拔电写坏的 catalog 会被当成“空盘”重建,8 本书的元数据一次性丢失
(书文件还在,但没有入口)。

要求:
- 存在但无效时:先把原文件 `rename` 成 `catalog.bad`(或 `catalog.bak`)再重建;
- 写入用 `catalog.tmp` + `fsync` 再替换(见下面 P0-3 对 FatFs rename 的注意);
- 读失败但文件存在时,`mp_store_init()` 应返回错误,让 app 决定是“降级只读”还是“重建”,
  不要静默清空。

#### P0-3 `flush_catalog_locked()` 非原子

`unlink()` 后 `open(O_CREAT)`(`mp_store.c:160-161`)之间掉电 → 没有 catalog。
任何一次 `set_flag/finish/delete/settings` 都会触发。

要求:写 `catalog.tmp` + `fsync`,再替换;至少在 `unlink` 前保留 `catalog.bak`。
注意 FatFs 的 `f_rename` 对“目标已存在”的行为与 POSIX 不同(IDF 的 `vfs_fat_rename`
直接调 `f_rename`),不要把 rename 当成 POSIX 原子覆盖;替换顺序要先确认所用 IDF/FatFs 版本的行为。

#### P0-4 所有失败都压成 `-1`,无法映射 BLE 状态码

`mp_store_*` 只返回 0/-1,`s_err` 是自由文本(`mp_store.c:53-60`);`ENOSPC`、`EIO`、
`ENODEV`(卡掉)、`EBUSY` 全部无法区分。而 Miaopaper 协议要求区分
`0x00 OK / 0x01 槽满 / 0x02 空间不足 / 0xFD 包长 / 0xFE 忙`(`ebook_ble.c:204-215`;
SURVEY `:320`),e2s_reader 也有 `ST_ERR_*` 分类(`storage.h:12-23`)。

要求:定义 `mp_err_t`(`OK/NO_SD/NO_SPACE/IO/BUSY/BAD_PARAM/NOT_FOUND`)或至少把 `errno`
带出来;`begin` 在 8 槽全 DONE 时返回“槽满”,写失败按 `errno==ENOSPC` 返回“空间不足”。

#### P0-5 与 `storage_remount()` 的生命周期冲突

`storage_remount()` 会 `storage_close_book()` + `st_up_force_close()` 后卸载卷
(`storage_sd.c:305-330`),但 `mp_store` 的 `s_wfd`、以及 lock/font/progress/settings 的
临时 fd 都不归它管。重挂后 `s_wfd` 指向已卸载卷上的 fd,继续写会失败或写坏。
另外两套锁互不认识:`storage` 用 `s_lock`(`storage_sd.c:66,102-134`),`mp_store` 用
自己的递归锁(`mp_store.c:49,62-75`),IDF FAT VFS 只在单次调用上加卷锁(`vfs_fat.c:394`)。

要求:
- 提供 `mp_store_sync_close()`(需新增:关 `s_wfd`、把 `UPLOADING` 归位),app 在
  `storage_remount()` 之前必须先调;
- 或让 mp app 在 SD 丢失时直接 `mp_store_abort()` + 不再重挂(重启恢复);
- 不要假设两个模块的写序列互不干扰。

#### P1-1 字库标志与真实状态会分叉

- `mp_store_init()` 只要内嵌字模存在就把 **RAM 里**的 `s_hdr[5]` 置 1(`mp_store.c:229-233`),
  **不落盘**;磁盘上的 `[5]` 可能还是 0。
- `mp_store_font_ok()` 不看这个标志,而是 `内嵌 || font.bin 大小正确`(`mp_store.c:559-565`)。
- `mp_store_font_begin()` `unlink` 后建 0 字节文件(`mp_store.c:567-576`),上传中途
  `font_ok()` 变 false,但磁盘 flag 仍是 1。

要求:二选一——(a) 删掉 flag,0x19 一律用 `mp_store_font_ok()`(内嵌字库永远在);
(b) init 时把 flag 落盘,并在 `font_begin` 时清 0、`font_finish` 时置 1。不要两套真相。

#### P1-2 lock/font/progress/settings 完全没有互斥,也没有完整 fsync

这些函数不取 `s_lock`(`mp_store.c:482-608,621-662,683-730`)。
`lock_finish()` 在另一个任务还在 `lock_write()` 时可能先写 `0xAA`,产生“标志有效但图不完整”。
`font_finish()` 只 `stat()` 不 `fsync`(`mp_store.c:595-608`);`settings_save()` 有 fsync
(`mp_store.c:724-727`),`progress_save()` 有(`mp_store.c:637-640`)。

要求:统一走同一把锁;`font_finish/lock_finish` 先 `fsync` 再写标志;所有“发布”动作
(写 flag/写 catalog)放在数据 fsync 之后。

#### P1-3 上传中再次 `begin` 会留下僵尸槽

`mp_store_begin()` 遇到 `s_wfd >= 0` 直接 `close()` 旧 fd,不改旧槽状态
(`mp_store.c:338-341`)。旧槽停在 `UPLOADING`,只能靠 `compact()` 清(`mp_store.c:436-450`)。
`find_free_slot()` 会把 `UPLOADING` 当空槽(`mp_store.c:306-320`),所以不会永久卡死,
但 0x15 书目可能短暂出现“上传中”的幽灵项。

要求:再 `begin` 前先 `abort()` 旧槽;或返回 `EBUSY` 让上层显式取消。

#### P1-4 进度值不校验范围

`mp_progress_save()` 接受任意 `uint32 pos`;`mp_progress_load()` 只校验 XOR
(`mp_store.c:621-662`)。XOR 能检出任何单比特错,但同一 bit 位上的偶数个错误会互相抵消
(例如第 0 位在两个字节里各翻一次),不是 CRC。
读到 `pos > book_len` 会把分页器推到文件尾。

要求:`save` 侧 `pos = min(pos, book_len)`;`load` 侧由 app 用 `mp_store_book_size()`
截断;不要把 0 当作“无进度”与“第 0 字节”混用(`delete` 用 0 清进度,`mp_store.c:432`)。

#### P1-5 标题编码与截断

`mp_store` 原样存字节(`mp_store.c:137-152`),不做 UTF-8→GB2312 转码;参考实现的
标题是 GB2312(网页端先转码,SURVEY `:124`),且**读标题时会做启发式修正**:
`enc != GB2312` 但出现 `>=0xA1` 字节时按 GB2312 画(`ebook.c:700-705`)。
`mp_font.c` 没有这个回退。

要求:BLE 收到的标题按 GB2312 存;`mp_store_read()` 或 app 显示前补同样的启发式;
19 字节截断可能切断双字节字符,截断时回退到字符边界。

#### P2 其他

- `mp_store_compact()` 只把僵尸槽重置为 `EMPTY`,不移动数据(`mp_store.c:436-450`)。
  名字容易误解为“整理书数据”。SD 上一书一文件,本就不需要搬数据——保留行为但在头文件写清。
- `mp_store_read_book()` 每次 `open/lseek/read/close`(`mp_store.c:452-472`),且取锁是
  `portMAX_DELAY`;上传期间每读一页都要和 `mp_store_write` 抢锁。分页读 1 页最多几十 KB,
  可接受,但 app 不应在 display 任务里调用。
- `mp_store_error()` 返回静态缓冲(`mp_store.c:53,244-247`),多任务同时读会互相覆盖。
- `mp_store_init()` 不检查 `storage_sd_ok()`,只 `mkdir`(`mp_store.c:220-223`);
  没有卡时错误是 `mkdir /sdcard/mp failed`,无法区分“没挂载”和“目录建不了”。
- `mp_store_find_free_slot()` 跳过 `s_wslot`(`mp_store.c:313`),但 `s_wslot` 只在 open
  成功后设置;失败路径已把槽置回 `EMPTY`(`mp_store.c:350-354`),行为可接受。
- `mp_store_set_flag()` 允许调用方写入 `DONE`(`mp_store.c:322-331`);若没有对应文件,
  0x19 会报有书、打开失败。调用方必须只用于状态迁移。
- `mp_settings_load()` 在记录无效时立即写默认值(`mp_store.c:701-702`);若 SD 只读/满,
  `mp_store_init()` 忽略它的返回值(`mp_store.c:238`),UI 会以为设置已保存。设置记录没有
  版本字段(只有 `"MPS1"` 魔数 + 4 个字段 + XOR),加字段必须换魔数或扩长度。
- `mp_store_init()` 重复调用会重新读盘并清 `s_wfd` 之外的 RAM 状态;`s_wfd` 不会被关,
  但正常流程只调一次。

### 2.3 线程模型小结

- 受保护:`s_entries`、`s_hdr`、`s_wfd`、`s_wslot`(递归锁,`mp_store.c:49,62-75`)。
- 不受保护:`s_ready`、`s_err`、progress/lock/font/settings 的文件操作。
- VFS 层:FAT VFS 对每次 `open/stat/unlink/rename` 加卷锁(`vfs_fat.c:394` 起),
  但 `unlink + open + write`、`lseek + write` 这种多步序列不原子。
- 结论:mp app 必须把所有 mp_store 调用收敛到**一个任务**(UI 任务)或自己再加一把锁;
  不要在 BLE 回调(host 任务)里直接调用 `mp_store_*`。

---

## 3. mp_font:渲染缺陷与边界

### 3.1 P0:比例字体的 blit 步长错误(已复现)

`mp_gfx_text()` 这样画一个字(`mp_font.c:51-56`):

```c
const GFXglyph *g = &font->glyph[c - font->first];
gfx_blit1(fb, cx + g->xOffset, baseline + g->yOffset, g->width, g->height,
          font->bitmap + g->bitmapOffset, (g->width + 7) / 8);
```

`gfx_blit1()` 的契约是“**每行** `stride` 字节”(`gfx.h:18-20`,`gfx.c:139-153`)。
但 `fonts/*.h` 来自 `oleddisplay.squix.ch` 生成器,字形是**位连续**打包、行间不补齐:

- `'!'`:`offset=1, w=2, h=12` → 24 bit = 3 B,下一条 `'"'` 的 offset 正好是 **4**;
  若按每行 1 B 会得到 1+12=13(`font16.h:101-104`)。
- `'"'`:`offset=4, w=5, h=4` → 20 bit = 3 B,下一条 `'#'` 的 offset 是 **7**
  (`font16.h:104-105`)。
- 正确的读法是 Adafruit GFX 的位指针:每 8 bit 前进 1 字节,不按行重置。

本次用同一份字体数据做了左右对比(左=现有 `mp_gfx_text`,右=位连续 blit),
左半 `Miaopaper 16px ABC 123` 完全重叠成一团,右半正常。**所有 ASCII 界面文字都会受影响**,
不只是标题。

修复建议:`mp_font.c` 内实现 `blit_gfx_bits()`(按 bit 计数推进),不要改
`gfx_blit1()`——`mp_font.c` 自己画 HZK 时也用它(`mp_font.c:69`,stride 2),
而且它是对外公开的 `gfx.h` 契约(“stride 是每行字节数”)。
若想复用,可在 `mp_font.c` 内把位图按行展开到临时数组,但每字 16×16 的临时栈开销没必要。

### 3.2 度量与绘制不一致

- `mp_gfx_width()` 对字体范围外的字节**不计宽**(`mp_font.c:30-35`),
  `mp_text_draw()` 的 ASCII 裁剪对范围外字节按 `adv=8` 计(`mp_font.c:108-111`)。
  例如 `0x7F`、`0x80..0xFF` 在 ASCII 模式:测量 0 px、裁剪按 8 px 推进但 `mp_gfx_text`
  不画(字体范围 0x20..0x7E),结果是“占位但不显字”。
- `mp_text_width()` 的 GB2312 分支对“单字节”一律记 16 px(`mp_font.c:79-90`),
  与绘制分支的 A3 全角一致,但最后一个字节是 `0xA1..0xFF` 时 `p[1]` 是 NUL,
  会走 else,被 `c > 0x7E` 改成空格,再按 A3 区 `0xA0` 去查字模 —— 该槽不合法,于是多画
  一个 16 px 空心框(`mp_font.c:133-146`)。不崩,但多一个字位。
- GB2312 模式把 `\r\n` 跳过但仍可能让 `p[1]` 越界读一位(NUL 在字符串内是安全的,
  但非 NUL 结尾的 buffer 不安全)。`mp_text_draw` 只接受 C 字符串,调用方必须保证 NUL。

### 3.3 其他边界

- `mp_text_draw()` 的 ASCII 裁剪缓冲只有 64 字符(`mp_font.c:104,118`),超出静默截断。
  标题 19 字符没问题,正文行若用它会丢字(正文应走 `text` 组件的分页器)。
- `MP_Y(y)` 只是 `y-6`(`mp_font.h:42-43`);`mp_text_draw`/`mp_hz_char` 自己不做
  这个平移,`mp_ui` 必须对所有 Miaopaper 坐标调用 `MP_Y`。y<6 会被 `gfx_*` 裁掉。
- `mp_hz_char()` 的 y 是**字形顶部**(`mp_font.c:61-70`),而 `mp_text_draw()` 的 `baseline`
  是基线,内部 `y = baseline - 12`(`mp_font.c:124`)。两套坐标不要混用。
- `GFXfont.bitmap` 是 `uint8_t *`(非 const),字体头把 `const uint8_t[]` 强转进去
  (`font16.h:198-199`)。只读没问题,但类型上可写,属于潜在 rodata 写崩点。
- 四份字体的 `bitmapOffset` 都远小于 65535(最大 `font_60.h` 约 8,135 B),
  `uint16_t bitmapOffset` 不会溢出。
- `mp_font.c` 为了 `MP_ENC_*` 常量 `#include "mp_store.h"`(`mp_font.c:12`)——渲染层不该依赖
  存储层。建议抽一个 `mp_types.h` 放 `MP_ENC_*`/`MP_TITLE_MAX`。
- 字模许可:`fonts/*.h` 来自 Miaopaper,文件头自己写着“internal builds only”
  (`mp_font.c:2-4`);`assets/NOTICE.md:39-56` 对 HZK16 的结论同样适用于此:对外发行前
  必须换掉/重新生成。详见 §11。

---

## 4. 5 键映射与 input 组件能力

### 4.1 物理映射

`board.h:46-54`:

| 逻辑键 | GPIO | 置信度 |
|---|---|---|
| `KEY_PREV` | 2 | **推断**(INVENTORY §4.2:GPIO2/4 仍为 PREV/NEXT 推断) |
| `KEY_NEXT` | 4 | **推断** |
| `KEY_FUNC` | 6 | **推断**(原 MENU 位;2026-11-25 真机只确认了 7/8) |
| `KEY_MENU` | 7 | **真机实测** |
| `KEY_OK` | 8 | **真机实测** |

- 低有效;`input_init()` 打开内部上拉(`input_keys.c:277-284`),原厂没开(INVENTORY §3.7),
  但对“另一端接 GND”的按键无副作用。
- 枚举顺序是 PREV/NEXT/MENU/OK/FUNC(`input.h:11-18`),`s_pins[]` 按枚举排
  (`input_keys.c:63-69`),不是 GPIO 升序——`board.h:49-53` 的宏顺序与枚举一致即可。
- **不要在 app 里把未验证的键绑到不可逆操作**(例如 GPIO6 长按直接恢复原厂)。
  PREV/NEXT/FUNC 的语义在第一次上机前都应可配置或有替代入口。

### 4.2 事件模型

`input_keys.c:201-255`:

- 20 ms 轮询(`input_keys.c:37`),`READER_KEY_DEBOUNCE_MS=60` → 连续 3 次一致才翻转
  (`reader_cfg.h:50`;`input_keys.c:38-43` 有整除静态检查)。
- 按下沿(去抖确认后)→ `KEY_EV_DOWN`,**没有“松开”事件**。
- 按住 ≥ `READER_KEY_LONG_MS` → `KEY_EV_LONG`,每次按下只发一次(`input_keys.c:240-247`)。
- 松开且总时长 < LONG → `KEY_EV_CLICK`,`held_ms` = 从去抖确认起算的时长
  (`input_keys.c:227-231`)。
- 已发 LONG 且仍按住的 **PREV/NEXT** → `KEY_EV_REPEAT`:首个在 LONG 之后
  `READER_KEY_REPEAT_MS=300` ms,之后每 300 ms(`input_keys.c:248-253`)。
  MENU/OK/FUNC **没有** REPEAT。
- 顺序:DOWN → (LONG → REPEAT...) 或 (CLICK);LONG 之后松开不会再发 CLICK。

### 4.3 `READER_KEY_LONG_MS=1000` 是否生效

**生效,已实测**。在 `/tmp` 构建的 `compile_commands.json` 里,`input_keys.c` 的编译命令包含
`-DREADER_KEY_LONG_MS=1000`(同样出现在 `mp_store.c`)。原因是
`e2s_mp/CMakeLists.txt:13` 在 `include(project.cmake)`/`project()` **之前**调用
`add_compile_definitions`,该目录属性会传播到随后 `add_subdirectory` 的组件。
`reader_cfg.h:51-53` 用 `#ifndef` 保护,不会被覆盖。

注意语义:计时起点是**去抖确认时刻**(`input_keys.c:220`),比物理按下晚约 60 ms,
所以真实 LONG 在按下后约 1.06 s;1.00 s 整松手可能得到 CLICK 而不是 LONG。

### 4.4 双击

**不支持**。`key_ev_type_t` 只有 4 个值(`input.h:20-25`),状态机没有任何多击计数。
Miaopaper 只有 F 键有双击(上一页,`ebook_buttons.c:67-70,155-163`),而且它的单击要等
~350 ms(`lwbtn.c` 的 multi-click 超时,SURVEY `:95-103`)。

若 e2s_mp 要复刻:
- 在 app 层记录上一次 `CLICK` 的时间;`t < 350 ms` 视为双击,吞掉第一次的翻页;
- 代价:所有单击翻页延迟 ~350 ms(与 Miaopaper 手感一致,但对 1 s 长按 + 20 ms 轮询的
  E2S 更明显);
- 建议按 Miaopaper SURVEY `:441` 的建议**去掉双击**,保留“短按翻页 + 长按退出/锁屏”,
  这样单击无延迟。

### 4.5 队列策略

- 固定 32 条线性数组(`reader_cfg.h:55`;`input_keys.c:73`),顺序 FIFO。
- 满时:
  - 新事件是 PREV/NEXT 且队尾同键 → 合并丢弃(`input_keys.c:119-123`);
  - 新事件是 PREV/NEXT 且队里有翻页事件 → 丢**最旧**的翻页事件,新事件入队,
    `input_dropped++`(`input_keys.c:124-131`);
  - 队列全是非翻页事件 → 丢新事件,`input_dropped++`(`input_keys.c:132-134`);
  - 非翻页事件 → 牺牲最旧的翻页事件;没有可牺牲的则最多重试 20 ms(2 ms 步进),
    仍失败计 `input_lost_critical++` 并 `ESP_LOGE`(`input_keys.c:137-148,152-183`)。
- `input_get()` 先取计数信号量再取锁弹出(`input_keys.c:337-365`);信号量在锁内 give,
  计数与队列长度一致(`input_keys.c:160-167`)。
- 结论:app 必须**及时消费**;一次 UI 卡顿 > 20 ms 且队列满,就可能丢非翻页键。
  但 32 条对 5 键人手足够,真正风险是 app 在 `handle_key` 里做同步 SD/刷新。

### 4.6 自测注入

`input_inject(id)` 线程安全,按“短按”注入:`PREV/NEXT → DOWN`,`MENU/OK/FUNC → CLICK`
(`input.h:43-48`;`input_keys.c:392-413`)。主机模拟器/自测应只用它,不要伪造 `key_event_t`
直接塞队列(队列是静态的)。

---

## 5. 内置 SD NAND:证据与含义

### 5.1 证据链

| 证据 | 出处 |
|---|---|
| SDMMC 4-bit,slot 1,CLK=11/CMD=12/D0=13/D1=14/D2=9/D3=10,内部上拉;`/sdcard` + FATFS;**GPIO34 电源/复位**(挂载前置低 50 ms 并保持低,卸载后置高) | INVENTORY `:38`(反汇编 0x42009bf4-0x42009c20、0x42009b34-0x42009b76、0x42009ce5) |
| GPIO34 完整时序复核:低=卡使能,高=断电/复位;每次重试都重新“置低 + 50 ms” | INVENTORY `:117-123` |
| 挂载 3 次/500 ms,失败只打印 `========> FATAL: SD card error <========` 并**继续启动** | INVENTORY `:39,135-136`(反汇编 0x420099a5-0x420099d7、0x42009ac4-0x42009acd) |
| 设备是“内部 flash + **外部 SD NAND**” | `hello_paperang/README.md:104` |
| 原厂代码注释直接写 “SD NAND on SDMMC bus” | `e2s_re/main/sdcard.c:2-6,21` |
| 书/字库/音频/口号/**OTA 镜像缓存**都在 `/sdcard`:1=`/sdcard/cache/ota`、6=`/sdcard/bk/bk0|bk1`、7=`/sdcard/cache/sta0|sta1`、10=`/sdcard/slogan`、12=`/sdcard/bk/ttf`、13=`/sdcard/bk/audio` | `e2s_re/components/ble/paperang_ble.c:43-44,350-357` |
| `word_manager` 直接从 `/sdcard/book0/bk0`、`/sdcard/bk/bk0|bk1` 读书 | `e2s_re/components/word_manager/word_manager_api.c:39-43`、`wm_m1_handle.c:214-215` |
| 音频资源 “live on SD NAND under /sdcard/bk/audio” | `e2s_re/components/paudio/player.c:8` |
| 8 MiB dump 里没有任何“insert card / no card / 请插卡”字符串 | 本次 `strings` 检查(§0.2 #12) |

### 5.2 含义

1. **它是常驻存储,不是可插拔卡**:原厂连 OTA 镜像都先落到 `/sdcard/cache/ota`
   (`paperang_ble.c:352`),书/字库/音频也全在 `/sdcard` 下——原厂把它当常驻可写存储用。
2. **UI 不能出现“请插卡”**:没有卡槽,用户无法“插”。正确文案是“存储不可用/存储错误/
   SD NAND 未就绪”,并给重试(重新挂载)入口。
3. **挂载失败必须继续**:原厂行为(INVENTORY `:135-136`)与 e2s_reader 的 `storage_init()`
   一致——失败返回 `ST_ERR_NO_SD`,`storage_*` 全部返回 `ST_ERR_NO_SD`
   (`storage_sd.c:288-298,119-134`),app 继续显示、BLE、恢复原厂。
4. **GPIO34 的代码映射**:`storage_sd.c:53-64` 默认
   `PULSE=0 / KEEP=0 / OFF=1`,`PULSE_MS=BOARD_SD_PWR_LOW_MS=50`,`SETTLE=10 ms`
   (`storage_sd.c:43,140-168`);`sd_mount_once()` 还显式把 `d4..d7` 置 `GPIO_NUM_NC`,
   因为 S3 的 SDMMC 默认 `d5=GPIO34` 正好是电源脚(`storage_sd.c:196-201`)。
5. **失败代价**:`esp_vfs_fat_sdmmc_mount(..., format_if_mount_failed=false)`
   (`storage_sd.c:205-211`)——**绝不格式化**。SD NAND 上若有原厂数据(字库/书/OTA),
   格式化会毁掉恢复路径。
6. **/sdcard/mp 是否合理**:
   - 合理:根目录下独立子目录,不与 `/sdcard/books`、`/sdcard/reader`、`/sdcard/bk`、
     `/sdcard/cache` 冲突;`CONFIG_FATFS_LFN_HEAP=y`/`MAX_LFN=255` 已开
     (`sdkconfig.defaults:19-20`),`catalog.bin` 等 8.3 名也合法。
   - 注意:如果同一张卡在两个固件(原厂 vs e2s_mp)之间来回用,`/sdcard/mp` 不会被原厂读,
     但也**不会**被原厂清理;卸载/重刷不会删它。这正好是 mp 的持久化位置。
   - 注意:mp_store 的 `MP_DIR` 是编译期字符串(`mp_store.c:25`),没有 `-D` 覆盖;
     主机模拟必须解决(§10)。

---

## 6. display:全刷/局刷/GC 与“更好策略”的保留方式

### 6.1 API

| API | 语义 | 出处 |
|---|---|---|
| `display_init()` | 只调一次;`epd_init` 失败/任务创建失败返回非 0,但 app 继续;`display_ready()` 为 false | `display.c:306-355`;`display.h:34-44` |
| `display_post(fb, mode)` | 拷 250×122、3904 B、行 32 B、1=白;latest-wins;返回后 app 可复用 fb | `display.c:365-377`;`reader_cfg.h:13-18` |
| `display_flush_wait(ms)` | 等队列空 + 面板 idle,超时也返回 | `display.c:379-395` |
| `display_request_full()` | 一次性强制全刷;不可被边界/GC 规则降级,但会等满 5 s | `display.c:426-436` |
| `display_set_gc(partials, idle_ms)` | 仅 RAM;`partials=0` 关闭自动 GC 并清 pending | `display.c:438-454` |
| `display_get_stats()` | 含 `gc_pending/baseline_valid/family/last_ms/last_full_ms` | `display.h:18-32` |
| `display_epd_diag()` | 驱动诊断一行文本(温度/ID/BUSY 轨迹/耗时) | `display.c:416-424`;`epd.c:588` |

### 6.2 刷新计划(必须理解,否则会“以为发了全刷”)

`plan_refresh()`(`display.c:79-189`):

1. 失败自愈:上一次失败且无新帧 → 用 `s_work` 重刷一次全刷,再失败不再自动重试
   (`display.c:100-109,216-221`)。
2. 有新帧:
   - `want_full = force || !baseline || mode==FULL || family!=1`(`display.c:113`);
     **`DISPLAY_MODE_PARTIAL` 不在这个表达式里**,所以它等价于 AUTO,不能“强制局刷”。
   - `family != 1` 一律全刷(`display.c:113,127`;`epd.c:539` `did_full = full || s_family != 1`)。
3. 5 s 间隔:
   - 只约束“上一次是局刷”之后的全刷;上一次就是全刷时允许立刻再全刷
     (`display.c:87-92`)。
   - 不满足时:若 `gc_pending && baseline && mode==FULL && !force` → 降级局刷
     (`why=boundary`,`display.c:140-151`);否则**等待**差额再全刷(`display.c:150,284-288`)。
4. GC 后 5 s:非强制的 FULL 且 `s_last_was_full && s_last_full_gc` → 降级局刷
   (`why=after_gc`,`display.c:133-138`)。
5. 空闲 GC:无新帧、`gc_pending && baseline && gc_partials!=0`,
   `since >= max(gc_idle_ms,5000) && gap_ok` → 全刷 `s_work`(`display.c:170-188`)。
   `gc_idle_ms` 默认 `READER_GC_IDLE_MS=45000`(`reader_cfg.h:43-45`);
   `display_set_gc` 存的是 u16;小于 5000 的值会在 `plan_refresh()` 里被抬到 5000,
   大于 65535 的毫秒值无法表示。
6. GC 计数:每次成功局刷 `partial_since_full++`,达到 `gc_partials` 置 pending
   (`display.c:222-230`);**任意全刷启动时**清 pending(`display.c:264-273`)。

### 6.3 面板族与温度

- `family`:复位后读 GPIO47,0→BOE(1,不读 ID),1→OED(3);`family=0` 表示未识别/初始化失败
  (`epd.c:512-519,578-581`;INVENTORY §1)。
- **只有 BOE 有局刷**(`epd.c:436-460`);OED 每次都全刷,~1.3 s/页
  (`USAGE.md:70-71`)。mp 的“局刷”设置在 OED 上无效,必须在设置页或日志里体现。
- **温度**:`epd_init()` 读一次 `0x40` 温度(`epd.c:261-283,378`),只存 `s_temp` 供
  `epd_diag()` 输出(`epd.c:590`);**不参与 LUT/波形选择**,也没有温度补偿。
  不要向用户承诺“随温度自动优化”。
- `epd_deep_sleep()` 是空操作(`epd.c:583-586`)——面板不会进深睡。

### 6.4 如何保留“空闲局刷和刷新策略更好”的优势

Miaopaper 的策略(`epd_refresh.c:20-67`):
- 场景进入 `force_next=1`;`epd_partial_enabled=0`(默认)时场景切换全刷并清 `partial_ready`;
- `epd_partial_enabled=1` 时按**全局**计数,每 N 次局刷一次全刷;
- 没有空闲 GC,没有 5 s 最小间隔,没有失败自愈。

e2s_reader 的 display 组件已经更强。mp 只需**正确映射**,不要另写一套:

| Miaopaper 概念 | e2s_mp 应做什么 |
|---|---|
| 场景进入(时钟/书架→阅读/菜单/INFO/锁屏/设置) | `display_post(fb, DISPLAY_MODE_FULL)`;若要求“一定全刷”(如锁屏、恢复页),先 `display_request_full()` 再用 FULL。注意 FULL 可能被 `boundary`/`after_gc` 降级 |
| 场景内翻页/移动 | `display_post(fb, DISPLAY_MODE_AUTO)`(或 `PARTIAL`,等价);不要每页 FULL |
| `epd_partial_enabled=0`(默认) | 只影响**场景进入**用 FULL;翻页仍是 AUTO。不要解释成“每页全刷” |
| `epd_partial_enabled=1` | 场景进入也用 AUTO(基线无效时 display 会自动升全刷);首次开机/打开书仍全刷 |
| `gc_interval_idx` | `display_set_gc(g_epd_gc_interval[idx], READER_GC_IDLE_MS)`;默认 `{10,20,50,100}`(`mp_store.c:46`) |
| 无空闲 GC | 不要关掉:保留 45 s idle GC(这是“更好”的核心) |
| 无 5 s 间隔 | 不要自己发 back-to-back 全刷;UI 上“手动全刷”要接受最多 5 s 等待 |
| 场景切换全刷的“仪式感” | 先画新场景再 post FULL;不要先 post 一帧旧内容再全刷 |

一个具体的坑:**“打开书”用 FULL 后立刻翻页**。display 会在全刷完成后允许局刷
(`s_last_was_full`),但 `nav_mode()` 式的“GC pending + 距上次 ≥5 s”判断在 app 层
(`reader_app.c:332-345`)——mp 若自己实现类似逻辑,必须用 `display_get_stats().last_ms`
(时刻,不是耗时,`display.h:20-25`)与 `rc_now_ms()` 同源比较。

另一个坑:`display_post()` 是 latest-wins。快速连按 NEXT 时,中间帧会被覆盖,但 app 的
页码状态仍要按每个 DOWN/REPEAT 前进;不要用“帧被显示”来驱动页码。

---

## 7. power:采样、充电脚、深睡与超时降级

### 7.1 采样链

- ADC1_CH0 = GPIO1;`BOARD_ADC_UNIT=0`(`ADC_UNIT_1`),`BOARD_ADC_CHANNEL=0`
  (`board.h:40-42`)。代码用 `adc_oneshot_io_to_channel(GPIO1)` 运行时断言 unit==0/ch==0,
  不匹配就置 `valid=false`(`power_battery.c:80-91`)——防的是“把 1 当 ADC1”导致
  实际选到 ADC2_CH0=GPIO11(SD CLK)(`power_battery.c:9-13`)。
- 12 bit、12 dB、curve fitting(`power_battery.c:105-130`)。
- 每次采样:GPIO45=1 → 等 `READER_ADC_SETTLE_MS=120` ms → 8 次 `adc_oneshot_read`
  + `adc_cali_raw_to_voltage` 平均 → GPIO45=0(`power_battery.c:245-267`)。
- `Vbat = pin_mv × 2`;`pct = clamp((Vbat+25-3500)*100/600, 0, 100)`
  (`power_battery.c:64-69,277-282`);`>4300 mV` 用历史值,首采超量程则 `valid=false`
  (`power_battery.c:139-158`)。
- `power_status_t` 还给出 `pin_mv`,便于和万用表比对(`power.h:11-22`)。

### 7.2 充电脚

- `BOARD_CHARGE_DET_GPIO=5`,输入,**低=charging**(`board.h:44`;`power_battery.c:71-75`)。
- 语义**未确认**(INVENTORY §8.5,`power.h:15`);代码默认开内部上拉
  (`POWER_CHARGE_DET_PULLUP=1`,`power_battery.c:45-53`),悬空读高=未充电。
  这只是为了让悬空有确定电平,不代表外部电路如此。第一次上机要实测充电/不充电两种状态,
  必要时 `-DPOWER_CHARGE_DET_PULLUP=0`。
- `power_sample()` 每次都会更新 `charging`(`power_battery.c:219`),但 UI 只应把它当提示,
  不要用它做充电管理(没有充电 IC 控制)。

### 7.3 低电策略

- `power_tick()` 每秒调用;每 60 s 采一次(`READER_BATTERY_PERIOD_MS`;
  `power_policy.c:66-75`)。
- 迟滞:warn <5% 置位、>7% 清除;critical <2% 置位、>5% 清除;无效读数**不改变**状态
  (`power_policy.c:29-64`)。
- 启动时 critical → ALERT 并拒绝 BLE 上传;阅读中只警告不禁止翻页
  (`USAGE.md §1.6`;`power_policy.c:11-12`)。
- **副作用**:`power_tick()` → `power_sample()` 会阻塞 ≥120 ms + 8 次 ADC
  (`power_battery.c:245-246`)。reader_app 在主循环每秒调一次
  (`reader_app.c:2291`),每 60 s 会有一次 >120 ms 的 UI 停顿。
  mp 若在意 1 s 长按/动画手感,应把 `power_tick()` 放到独立任务,或接受这个停顿并
  在采样窗口内不刷新。

### 7.4 深睡可用性:不可用

三层证据:
1. `power_enter_deep_sleep()` 无条件 `return ESP_ERR_NOT_SUPPORTED`
   (`power_battery.c:293-301`),`READER_ENABLE_DEEP_SLEEP=0`(`reader_cfg.h:68`)。
2. `epd_deep_sleep()` 只打印 `deep sleep skipped (r22)`(`epd.c:583-586`);
   原厂的 `0x07+0xA5` 深睡/唤醒路径未在样例中验证(INVENTORY `:458`)。
3. sdkconfig 没有 `CONFIG_PM_ENABLE`,display 任务每 1 s 被 `s_wake` 超时唤醒
   (`display_internal.h:18-20`;`display.c:279-281`),FreeRTOS tick 也不会停。

**因此 mp 的 15/30/60/120 s 睡眠必须降级**,建议语义:
- 超时 → 画锁屏图(若 `mp_store_lock_valid()`)或“待机/时钟”页,用 FULL 刷新
  (先 `display_request_full()`;若刚翻过页会被 5 s 间隔挡住,需接受等待);
- 停 BLE 广播(参考 `READER_ADV_IDLE_MS=120000` 的现有行为,`reader_app.c:2308-2312`);
- 不再读 SD(不关卷也行;若要关,先 `mp_store_sync_close()`);
- 任意键/连接 → 恢复原场景并重新开广播;
- **不要**调用 `esp_deep_sleep_start()` 或 `power_enter_deep_sleep()` 并假装成功;
  也不要在 UI 里写“已休眠/省电模式”,除非实测电流。
- 若未来启用真深睡,前置条件见 `DESIGN.md:699-701`:进度/书签 fsync、面板 `0x10←0x01`、
  5 键上拉 + GPIO 唤醒实测、唤醒后 `baseline_valid=false` 首帧全刷。
  当前 `epd_deep_sleep()` 未实现,所以“面板深睡”也不存在。

---

## 8. recovery:恢复原厂槽

### 8.1 API 与约束

`recovery.h:23-38`:

```c
recovery_rc_t recovery_stock_probe(char *detail, size_t detail_len);   /* 只预检,不写 */
recovery_rc_t recovery_stock_commit(char *detail, size_t detail_len);  /* 重新预检后提交 */
```

返回:`OK / NO_STOCK / LAYOUT / UNSUPPORTED / WRITE / VERIFY`。

约束(全部有代码证据):
- 目标槽必须是**已存在**的有效 app:magic `0xE9`、`esp_image_verify()`、
  appended SHA-256、app descriptor 的 `project_name == "paperang_e2"`
  (`recovery.c:114-158`,常量 `:48`;原厂身份见 INVENTORY `:73`)。
- **调试器连接时拒绝**(`esp_cpu_dbgr_is_attached()`,`recovery.c:123-126`),因为 JTAG 下
  app 构建会跳过完整性检查。
- secure boot / flash encryption / anti-rollback 任一开启 → `UNSUPPORTED`
  (`recovery.c:217-227`)。
- 必须正好两个 OTA 槽 + 一个 0x2000 的 otadata,且不与任何分区重叠
  (`recovery.c:91-109,241-310`)。
- 只擦写“**不持有最高可用 seq**”的那个 4 KiB 扇区,保留一条可启动回退
  (`recovery.c:160-188`;`recovery_seq.h:18-63`)。
- 写入后回读 32 B + 用 bootloader 规则复核选槽(`recovery.c:371-398`);
  若写后复核读失败但回读一致,返回 OK 并提示“已写入,重启后生效”(`recovery.c:383-388`)。
- **不接收、不写入任何 app 镜像**;全仓库只有这里调用
  `esp_partition_erase_range/write`(`recovery.c:358,364`;本次 grep 证实)。
- 编译期禁止 OTA rollback/anti-rollback(`recovery.c:38-40`)。

### 8.2 UI 接入点(可照抄 reader_app)

`reader_app.c` 的流程是一个可复用的最小实现:

1. 入口:`APP_LIBRARY`/`APP_ALERT` 下 **FUNC 长按** → `restore_enter()`
   (`reader_app.c:1339` 书架/`:1422` ALERT;`restore_enter` 在 `:657-680`)。
2. `restore_enter()` 先 `recovery_stock_probe()`;失败直接显示原因,**不写 flash**。
3. `restore_present()` 画确认页 → `display_flush_wait(4000)` → 要求
   `after.frames > before.frames` 才允许 OK(`reader_app.c:633-655`)。
   这是“没真正上屏就不让确认”的失败关闭设计。
4. 确认 → 保存进度(best effort)→ `recovery_stock_commit()` → 成功页 →
   `display_flush_wait(4000)` → 3 s → `esp_restart()`(`reader_app.c:696-730`)。
5. 取消/MENU 返回原状态(`restore_cancel`)。

对 e2s_mp 的要求:
- 恢复入口必须在**无 SD、无书、display 正常**时也能到达;`display_ready()` 为 false 时拒绝
  (`reader_app.c:663-666` 的失败关闭)。
- 恢复页必须用 FULL(必要时 `display_request_full()`),并等真正刷新完成再开放 OK。
- 只写一条 otadata;不提供“任意镜像切槽”“OTA 下载”。
- 提醒:切到原厂是**单向**的;要回到 mp,通常需要原厂 OTA/刷机工具把 mp 镜像写回另一槽
  (原厂把 OTA 包缓存在 `/sdcard/cache/ota`,`paperang_ble.c:352`;具体写哪个槽未验证)。
  UI 文案应说明这一点。

### 8.3 未运行

`recovery` 的纯函数有主机测试(`run_host_tests.sh` 里 `recovery: 92 checks, 0 failure(s)`),
但**没有**在真机上 probe/commit 过;`esp_image_verify`、otadata 写入、重启后选槽都未验证。

---

## 9. 构建与烧写边界(ESP-IDF v6.1.1)

### 9.1 版本与依赖

- 本机 IDF:`~/esp/esp-idf-v6.1.1`(另有 v4.4.4,不要混用);工具链
  `xtensa-esp-elf 15.2.0_20251204`(本次构建日志)。
- e2s_mp 顶层:`cmake_minimum_required 3.16`、`EXTRA_COMPONENT_DIRS ../e2s_reader/components`、
  `add_compile_definitions(READER_KEY_LONG_MS=1000)`、`project(e2s_mp)`
  (`e2s_mp/CMakeLists.txt:7-16`)。
- `sdkconfig.defaults` 与 e2s_reader 同硬件配置:8 MB DIO 80 MHz、无 PSRAM、240 MHz、
  FreeRTOS 1000 Hz、SIZE 优化、主任务栈 6144、任务看门狗 10 s、自定义分区表、
  FATFS LFN、NimBLE(无 NVS 持久化、无 bonding)、关 PHY calib 存储、关 WiFi NVS
  (`e2s_mp/sdkconfig.defaults:3-33`)。
- `partitions.csv` 与 e2s_reader **逐字节相同**(本次 `diff` 无差异):nvs 0x9000/0x6000、
  otadata 0xF000/0x2000、phy_init 0x11000/0x1000、ota_0 0x20000/0x1F0000、
  ota_1 0x210000/0x1F0000。
- 关键:v6.1.1 把旧 `driver` 拆成 `esp_driver_gpio`/`esp_driver_spi`/`esp_driver_sdmmc` 等,
  复用组件的 CMake 已按新名字写好;本次全量构建通过,说明没有遗漏。

### 9.2 main/CMakeLists.txt 的 REQUIRES

最小:`common text display storage input power`;用到 BLE 加 `ble`;用到恢复加 `recovery`。
不要 `REQUIRES driver`(v6 里是聚合兼容层,容易带进不需要的驱动)。
`mp_store.c` 通过 `REQUIRES text` 拿到 `text_hz16_glyph`;`mp_font.c` 通过 `display` 拿到
`gfx_*`。

### 9.3 build.sh 的安全边界

`e2s_reader/tools/build.sh`(直接复制到 `e2s_mp/tools/` 即可:`ROOT` 由脚本自身路径推导,
会自动指向 e2s_mp,无需改路径):

- 拒绝参数中出现的任何 `flash | app-flash | bootloader-flash | partition-table-flash |
  otadata-flash | encrypted-flash | erase-flash | dfu | uf2`(逐参数扫描,不只看 `$1`)
  (`build.sh:27-34`)。
- 只执行 `idf.py build/size/...`;默认 `build`(`build.sh:23-25,39-40`)。
- IDF 路径默认 `$HOME/esp/esp-idf-v6.1.1`,可用 `IDF_PATH` 覆盖(`build.sh:16-21`)。
- **它不会烧写**;烧写必须用 `flash_app.sh` 打印的命令。

`flash_app.sh` 的边界:
- 只打印命令,不打开串口、不调用 esptool(`flash_app.sh:1-13`)。
- 只允许 0x20000 / 0x210000;镜像 >0x1F0000 退出码 3 且不打印(`flash_app.sh:92-97`)。
- 明确列出禁止写:0x0、0x8000、0x9000、0xF000、0x11000、0x100000、0x200000、0x480000
  (`flash_app.sh:14-24`)。
- **复制到 e2s_mp 时必须改默认镜像名**:脚本里 `IMAGE="${IMAGE:-$ROOT/build/e2s_reader.bin}"`
  (`flash_app.sh:84`),否则会打印一个不存在的路径(或烧错镜像)。
- 它只决定打印哪个槽,不读 otadata;活动槽要人工确认(`flash_app.sh:30-32`)。

### 9.4 镜像尺寸

- 本次带桩构建:`build/e2s_mp.bin` = **537,792 B**,槽上限 2,031,616 B,余量充足。
- e2s_reader 的完整构建是 891,376 B(`USAGE.md:146`),mp 加 BLE+UI 后仍应在 1 MB 量级。
- `CONFIG_PARTITION_TABLE_CUSTOM=y` 只影响构建期偏移;**不代表 `idf.py flash` 安全**
  (`partitions.csv:8-12`;`USAGE.md:178-179`)。

### 9.5 不要做的事

- 不要运行 `idf.py flash`(会写 bootloader 0x0、分区表 0x8000、otadata 0xf000)。
- 不要 `erase-flash`;不要写 NVS/phy_init;不要改 otadata(除 recovery)。
- 不要把 `partitions.csv` 改成 `SINGLE_APP_LARGE` 之类来“方便构建”——运行时表来自 flash,
  编译期表只需要偏移正确、镜像不超 0x1F0000。

---

## 10. 主机模拟可行性

### 10.1 哪些能编(已实测)

| 文件 | 结果 | 条件 |
|---|---|---|
| `components/display/gfx.c` | ✅ | 只需 `-I common/include -I display/include`;文件自身已 `#include <stdbool.h>`(`gfx.c:7`)。`run_host_tests.sh:38-41` 的 `-include stdbool.h` 是历史遗留,可保留 |
| `components/text/text_font.c` + `text_encode.c/layout/page/font8x16.c` | ✅ | `-DTEXT_HOST_BUILD` + 资产注入;现有 `run_host_tests.sh` 已跑 985,765 检查 |
| `e2s_mp/main/mp_font.c` | ✅ | 同上 + `-I e2s_mp/main`;`-Werror` 通过 |
| `e2s_mp/main/mp_store.c` | ✅(需桩) | 3 个桩头:`esp_log.h`、`freertos/FreeRTOS.h`、`freertos/semphr.h`;`-D_GNU_SOURCE` |
| `e2s_mp/main/mp_ui.c` | ❌ | **文件不存在**,无法验证;实现时必须只依赖 `gfx.h`/`mp_font.h`/`<stdint.h>` 才能保持可编 |

### 10.2 资产路径

- `components/text/assets/HZK16_87.bin` — 261,696 B,
  SHA-256 `675f3d39c67ec4d9b7192b581fd589ab8367bac2d89a90f59695c4383d54602d`
  (本次 `sha256sum`;与 `hz16_gb2312.bin` 逐字节相同)。
- `components/text/assets/gb2312_map.bin` — 29,792 B,
  SHA-256 `2f6a86312594565f764888dd46f59a58985e5b1769808fe3633f177b44da95b2`。
- 固件里由 `text/CMakeLists.txt:19-43` 以 `EMBED_FILES` 内嵌;缺失时 **configure 期 FATAL_ERROR**。
- 主机上 `mp_font`/`text_font` **不读磁盘**:必须先 `text_assets_set_host(hz16, len, map, len)`
  (`text_font.c:147-158`),该函数只在 `TEXT_HOST_BUILD` 下声明(`text_internal.h:104-107`),
  是**私有头**。模拟器要么包含 `text_internal.h`(允许,但要知道它是私有 API),
  要么在自己的 harness 里链接 `text_font.c` 并调用它。
- `mp_store` 不读字库文件,只 `text_hz16_glyph()` 探针;主机上给一个返回非 NULL 的桩即可
  让 `font_ok` 为真,或链接真实 `text_font.c` + 注入资产。

### 10.3 PNG / contact sheet(已实测)

- Pillow 12.3.0:可从 PGM 读,或 `Image.frombytes("L", (250,122), buf)`(`buf` 每像素 1 B)。
- ImageMagick 7.1.2-32:`magick frame.pgm -scale 300% frame.png` 成功;
  `montage` 也可用(用于多页 contact sheet)。
- 本次已生成 `/tmp/mp_probe/frame.png`(单帧)与 `/tmp/mp_probe/cmp.png`(左右对比);
  PGM 是最省事的中间格式:`P5\n250 122\n255\n` + 每像素 1 B(255=白,0=黑)。
- 注意极性:逻辑帧 1=白、0=黑(`reader_cfg.h:13-14`);`gfx_clear(fb, 0)` 得到**白底**
  (参数名是 black,0=不画黑)。写 PGM 时要把 bit 1 映射成 255。

### 10.4 模拟器需要的桩

| 模块 | 桩 | 说明 |
|---|---|---|
| `display` | `display_post()` 保存 3904 B 到全局;`display_ready()` 返回 true;`display_get_stats()` 返回可控结构 | 不要在主机上调用 `display_init()`(会拉 `epd_init`/SPI) |
| `input` | 用 `input_inject(id)`;或直接构造 `key_event_t` 喂 app 的 `handle_key` | 主机上不要启动 `input_task`(`gpio_get_level` 不存在) |
| `power` | `power_get()` 返回固定 `{mv,percent,valid,charging}`;`power_tick()` 空实现 | 避免 120 ms 阻塞 |
| `storage` | 把 `/sdcard` 换成临时目录;`storage_sd_ok()` 返回 true/false 两种模式 | 用于测“无存储”UI |
| `mp_store` | **必须解决 `MP_DIR` 写死**:要么给 `mp_store.c` 加 `#ifndef MP_DIR` 覆盖,要么在主机上以 root 建 `/sdcard` 或 bind mount | 本次实测:未改源码时 `mp_store_init()` 返回 `mkdir /sdcard/mp failed` |
| `text` | `text_assets_set_host()` + 两份 bin | 见 10.2 |
| `recovery` | 已有主机测试 `tools/test_recovery_host.c`;UI 侧用“总是 NO_STOCK”的桩测失败页 | 不要在主机上执行 commit |
| `ble` | 把 `reader_ble_init/notify/adv` 换成记录调用的桩;命令用 `reader_ble_set_cmd_cb` 注入 | 不要在主机上初始化 NimBLE |
| FreeRTOS | `xTaskCreate*`/`vTaskDelay`/信号量 → pthread 薄封装 | `mp_store` 需要递归互斥;`display` 需要 binary semaphore + task |

### 10.5 推荐架构

- 一个 `main_host.c`:解析 `--frame N`、`--input KEY`、`--store DIR`,调用 `mp_ui` 的纯函数
  `mp_ui_render(state, fb)`,输出 PGM/PNG。
- 把 UI 拆成“纯渲染 + 事件归约”两层:渲染只吃 `const state *` 和 `uint8_t *fb`
  (250×122),不碰 ESP 头;事件处理只改 state,不直接刷新。这样 `mp_ui.c` 可以在
  gcc 下编译,也能在固件里复用。
- 回归:对每个场景生成 PNG + 像素哈希;对文本用 `mp_text_width()` 断言行宽;
  对 `mp_store` 用临时目录跑 begin/write/finish/delete/compact 的正常与掉电(截断)用例。

---

## 11. 安全红线

以下每条都有仓库内证据,实现时按“硬约束”处理:

1. **只写 app 槽**:0x20000 / 0x210000,镜像 <0x1F0000;绝不写 0x0、0x8000、0x9000、
   0xF000、0x11000、0x480000(`flash_app.sh:10-24`;INVENTORY `:447-448`;
   `USAGE.md:175-179`)。
2. **不写 bootloader/分区表/NVS/phy_init**:复用组件里没有任何 `nvs_*` 调用(本次 grep);
   全仓库只有 `recovery.c:358,364` 写 flash,且目标是 otadata。
3. **otadata 只允许 recovery**:`recovery_stock_commit()` 是唯一入口;不要在 app 里直接
   `esp_partition_write(otadata)` 或 `esp_ota_set_boot_partition()`(后者全仓库未使用)。
4. **不用 `idf.py flash`**:它会写 4 条(0x0/0x8000/0xf000/app)(`USAGE.md:239-251`)。
5. **不打印设备身份**:NVS 里的 `sn/key/bind/cal_data/cal_mac` 是身份
   (INVENTORY `:449`);不要 `esp_read_mac()` 打印 MAC,不要把 MAC 放进 BLE 名/日志。
   现有 BLE 只打印 `addr_type`(`reader_ble.c:535`),设备名是固定的 `E2S-Reader`
   (`ble_proto.h:26`)——保持这样。
6. **不把 HZK16 数据复制进新仓库**:e2s_mp 现在没有 `assets/`,依赖 `text` 组件的内嵌资产;
   不要新建 `e2s_mp/main/fonts/HZK16_87.bin` 或把 `hz16_gb2312.bin` 拷进 mp 目录。
   `NOTICE.md:39-56` 明确:当前内嵌字模与 HZK16_87.bin 逐字节相同,**只限内部/自用**;
   对外发行前必须 `tools/gen_hz16.py --from-ttf <OFL/Apache TTF>` 重新生成。
   同理 `fonts/font16*.h`/`font30.h`/`font_60.h` 的许可不明(`mp_font.c:2-4`)。
7. **不写原厂字库区 0x480000–0x528884**(INVENTORY `:41,78`;`NOTICE.md:60`)。
8. **SD 失败不中止**:显示“存储不可用”,保留 BLE 与恢复通道(INVENTORY `:452`)。
9. **BLE 失败/未连接不得阻塞启动**:面板与 UI 必须独立启动(INVENTORY `:451` 的反面例子)。
10. **不做未验证的深睡**:见 §7.4;进深睡前必须有进度 fsync、面板深睡、GPIO 唤醒实测。

---

## 12. 实现者必须遵守的接口/约束清单

### A. 构建与工程

1. 新建 `main/CMakeLists.txt`,`idf_component_register(SRCS "mp_store.c" "mp_font.c" <app...>
   INCLUDE_DIRS "." REQUIRES common text display storage input power [ble] [recovery])`。
2. 新建 `app_main`;IDF 不会因为没有 `app_main` 给出友好错误,只会在链接期失败。
3. 不要修改 `../e2s_reader/components` 的公共头/CMake(冻结);mp 专属逻辑放 `main/`。
4. 复制 `tools/build.sh` 到 `e2s_mp/tools/`(拒绝 flash 子命令的名单必须保留);
   复制/改写 `flash_app.sh` 时把默认镜像改成 `build/e2s_mp.bin`(`flash_app.sh:84`)。
5. 不要加 `CONFIG_PARTITION_TABLE_SINGLE_APP_LARGE`;`partitions.csv` 保持与真机一致。

### B. mp_store

6. `mp_store_init()` 之前必须 `storage_init()` 成功;失败时不要调用 `mp_store_*`,
   直接进“存储不可用”UI。
7. 所有 `mp_store_*` 调用收敛到一个任务/一把锁;不要在 BLE host 回调里直接调用。
8. `mp_store_begin()` 前先确认 `mp_store_find_free_slot() >= 0`,并把“8 槽全 DONE”
   映射成协议状态 `0x01`;写失败按 `errno==ENOSPC` 映射 `0x02`。
9. `mp_store_write()` 必须按声明长度做边界检查;`finish` 必须 `stat` 比对;
   不满足就 `abort`,不得置 `DONE`。
10. catalog 写入用临时文件 + `fsync` 再替换(先保留 `catalog.bak`;FatFs rename 不是
    POSIX 原子覆盖);读失败不要把现有 catalog 清空,先备份再重建。
11. 字库标志只保留一套真相:要么永远用 `mp_store_font_ok()`(内嵌字库常在),
    要么 init 落盘/begin 清 0/finish 置 1。上传的 `font.bin` **不参与渲染**,
     UI/协议不要暗示“上传后字形会变”。
12. `mp_store_lock_read()` 返回的是 **4000 B `epd_temp` 布局**(`idx=(y>>3)*250+x`,
    `bit(y&7)`,1=黑),不是 250×122 逻辑帧;显示前必须转置 + 反相,
    并丢弃 y=122..127 的 6 行。不要 `memcpy` 到 fb。
13. `mp_progress_save()` 的 `pos` 先按 `mp_store_book_size()` 截断;`pos==0` 表示“从头/清空”,
    不要和“无记录”混用。
14. 标题按 GB2312 字节存;显示时补 `>=0xA1` 的启发式(`ebook.c:700-705`);
    19 字节截断要回退到双字节边界。
15. `storage_remount()` 之前必须调用新增的 `mp_store_sync_close()`(关 `s_wfd`、把
    `UPLOADING` 归位);否则 fd 跨卸载存活。
16. 只有 `/sdcard/mp` 可写;不要碰 `/sdcard/books`、`/sdcard/reader`、`/sdcard/bk`、
    `/sdcard/cache`、NVS、任何分区。

### C. mp_font / UI

17. 修 `mp_gfx_text()` 的 blit:按位连续读(每 8 bit 前进 1 字节),不要改
    `gfx_blit1()` 的“按行 stride”契约(`mp_font.c:69` 的 HZK 用 2 B/行,且它是公开 API)。
18. 所有 Miaopaper 坐标先 `MP_Y(y)=y-6`;`mp_hz_char` 的 y 是顶部,`mp_text_draw` 的
    baseline 是基线(`y=baseline-12`),不要混。
19. 逻辑帧:250×122、行 32 B、1=白、0=黑;`gfx_clear(fb, 0)` 是白底;
    只通过 `display_post()` 上屏,不要直接调 `epd_*`。
20. `mp_text_draw` 只用于短标签;正文用 `text` 组件的分页器(`text_page_forward/backward` +
    `text_render_page`),不要用 64 字符裁剪缓冲。
21. 不要新增 HZK16/字体二进制到 e2s_mp;沿用 `text` 组件的内嵌资产。

### D. input

22. 5 键映射按 `board.h:49-53`;GPIO7=MENU、GPIO8=OK 已实测,PREV/NEXT/FUNC 未验证,
    不要在其上绑定不可逆操作。
23. 事件消费:DOWN/CLICK/LONG/REPEAT;PREV/NEXT 的 DOWN 就要翻页,REPEAT 只做自动重复;
    MENU/OK/FUNC 的短按只认 CLICK。
24. 没有双击;若要 F 双击=上一页,app 层用 CLICK 间隔 <350 ms 合成,并接受单击延迟。
25. 不要在按键处理里做同步 SD 读写或等刷新;队列满 20 ms 就会丢非翻页键。
26. `READER_KEY_LONG_MS=1000` 已生效;不要再在代码里写死 800/1000。
27. 自测用 `input_inject()`;不要伪造 `key_event_t` 直接写队列。

### E. display

28. 场景进入用 FULL(必要时先 `display_request_full()`);场景内翻页/移动用 AUTO;
    不要每页 FULL。
29. 不要指望 `DISPLAY_MODE_PARTIAL` 强制局刷——它等价 AUTO。
30. 手动全刷/锁屏全刷要接受最多 5 s 等待(display 会凑间隔);UI 要有“刷新中”状态。
31. 保留空闲 GC:用 `display_set_gc(g_epd_gc_interval[idx], READER_GC_IDLE_MS=45000)`;
    不要把 `partials` 设为 0 除非用户明确关闭自动 GC。
32. 用 `display_get_stats().last_ms`(时刻)与 `rc_now_ms()` 同源判断 5 s 边界,
    不要用耗时/本地计时器。
33. 非 BOE(`family != 1`)时每页全刷(参考 BOE 全刷 ~1.3 s,OED 未测);
    设置页/日志要显示 family,不要承诺局刷速度。
34. 冷启动第一帧必须全刷(baseline 无效时 display 会自动做,但 app 不要先 post 一帧
    局刷内容)。

### F. power

35. 电量每 60 s 采一次;`power_tick()` 会阻塞 ≥120 ms,放在独立任务或接受停顿。
36. 低电只做提示(<5%)与启动 critical(<2%,拒绝上传);不要禁止翻页。
37. 充电脚 GPIO5 低=charging,语义未确认;只作提示,不要据此控制充电。
38. **不要实现/调用深睡**;sleep 超时降级为锁屏/待机 + 停广播 + 停 SD,任意键恢复。
    不要写“已休眠”。

### G. recovery

39. 恢复入口在无 SD/无书时也可达;`display_ready()` 为 false 时拒绝(失败关闭)。
40. 先 `recovery_stock_probe()`,把 `detail` 显示给用户;确认页必须真正上屏
    (`display_flush_wait` + 帧数增加)才开放 OK。
41. 确认前保存进度;成功后 `esp_restart()`;失败留在失败页,不重启。
42. 不提供镜像写入/下载/任意槽切换;不改 otadata 的其他字节。

### H. 安全

43. 不打印 MAC/SN/efuse/NVS 身份;BLE 名固定,不带唯一信息。
44. 不写 NVS/phy_init/bootloader/分区表/原厂字库区;只写 app 槽与 `/sdcard/mp`。
45. 不把 HZK16 数据复制进新仓库;对外发行前用 `--from-ttf` 重新生成字模。

---

## 13. 未验证与风险(实现时要防御)

1. **SD 引脚/GPIO34 只在反汇编层面确认**,没有真机挂载记录(INVENTORY `:467`);
   第一次上机应先用 `sdmmc_card_print_info` 验证容量/位宽。
2. **SD NAND 的容量与剩余空间未知**;`ENOSPC` 必须可区分并映射到协议 `0x02`。
3. **OED 面板无局刷**;若目标批次 `fam=3`,每页 1.3 s。上线前看启动日志的 `epd discern ... family=`(`epd.c:518`)。
4. **按键 PREV/NEXT/FUNC 映射仍是推断**;GPIO6 的 FUNC 语义没有真机确认。
5. **充电脚语义未确认**;内部上拉是权宜。
6. **深睡/唤醒完全未验证**;本版本不要启用。
7. **recovery 未在真机 commit**;`esp_image_verify` 对原厂镜像的行为、写后重启选槽都未实测。
8. **mp_store 的掉电行为未测**;catalog 非原子、font 无 fsync 是已知缺口。
9. **`/sdcard` 与两个固件共享**:原厂可能重建 `/sdcard/bk`、`/sdcard/cache`;
   mp 只用 `/sdcard/mp`,但不要假设整卡归自己(不要 `f_getfree` 后写满)。
10. **字体许可**:当前内嵌 HZK16 与 `HZK16_87.bin` 逐字节相同,只限内部;
    `fonts/*.h` 许可不明;对外发行前必须替换。
