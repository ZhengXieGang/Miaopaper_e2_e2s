# DESIGN: e2s_mp 架构与实现说明

配套文档：`README.md`（简介、刷入）、`docs/BUILD.md`（构建、手动刷写）、
`docs/PORT_SPEC.md`（冻结施工规范）、
`docs/COPY_DECK.md`（逐字文案）、`docs/KEYMAP.md`（按键矩阵）、
`docs/PROTOCOL.md`（BLE 协议）。

本文按当前代码描述。规范与代码冲突时以代码为准，仍在的限制见 §11。
本轮未连接真机，验证范围见 §12。共享组件路径（`components/...`、
`reader_cfg.h`、`power_battery.c`、`epd.c`）均相对 `../e2s_reader/`。

---

## 1. 目标与约束

### 1.1 三条来源

| 来源 | 取什么 | 不取什么 |
|---|---|---|
| 原厂 1.3.739 | 三段式栅格、16 px 正文、8 px 边距、键帽图标、电量图标 | 触屏宫格、弹层、30 px 首页大标题、商业字体、插图 |
| Miaopaper | 五键手势、350 ms OK 多击门、分页规则、8 槽 32 B 目录、`0x10..0x44` BLE 协议、4000 B 锁图、HZK16 | 外部 NOR flash 布局、128-bit EPD 推屏服务、图片和音频商店 |
| E2S 平台 | 内置 SD NAND、ESP32-S3、250×122 1bpp、5 键、电量与充电脚、otadata 双槽 | 深睡（不可用）、RTC（不存在）、SD 卡槽（不存在） |

### 1.2 硬约束

- `mp_store` 只写 `/sdcard/mp`。它不写 NVS、内部 flash、分区表、phy_init 或
  原厂字库区，也不格式化 SD。启动期共享 `storage_init()` 会创建
  `/sdcard/books` 和 `/sdcard/reader/{state,tmp}` 并扫描 `/sdcard/books`。
  这些目录由平台准备，mp 不写其中的书或进度内容，见 §8.1。
- 只有 `recovery_stock_commit()` 写 otadata。app 不接收、不写任何 app 镜像。
- 所有状态归 app 任务：按键归约、`mp_store_*`、`display_post()` 和恢复流程
  都在 app 任务。BLE host 只把包拷进队列。
- 渲染与业务解耦：`mp_ui.c` 只吃视图模型和帧缓冲，不读 SD、不碰按键。
  `mp_page.c` 只做分页测量。`mp_store.c` 不碰 UI 或 BLE。
- 开启深度省电架构：CPU DFS (40–240 MHz) + FreeRTOS Tickless Idle 自动
  Light Sleep；锁屏且无蓝牙超时或低电时进入 `esp_deep_sleep_start()`，
  EPD 调用 `display_sleep()` 保留画面，长按 `○` 键 500 ms 无感恢复现场。

### 1.3 十七个屏幕

`mp_screen_t` 有 17 个值：

```
CLOCK SELECT READ SETTINGS ABOUT LOCK            6 个主屏
EMPTY ERROR RESTORE NO_LOCK_IMG LOW_BATT         5 个状态/关机页
BOOK_INFO JUMP DELETE TIME_SET TIME_DONE BACKLIGHT 6 个工具/设置页
```

它们共用同一套状态栏、栅格和底栏规则（`LOCK` 和 `LOW_BATT` 整屏无状态栏/底栏），区别在内容、可用按键和是否画时间。
`MP_SCREEN_BACKLIGHT` 提供 4 档（0..3）AW2023 背光调节并支持常规屏长按胶囊菜单键 `▭` 呼出快捷浮窗；开机先以 `i2c_master_probe(0x45)` 只读探测芯片在位并核对 ID `0x09`，未检测到背光芯片（E2）时立即释放 I2C 总线并进入无背光自适应模式（隐藏设置背光项、禁用快捷浮窗、关于页显示 `Miaopaper E2`）；
`MP_SCREEN_LOW_BATT` 在未充电且电量 `< 3%` 时全刷空电池与“请充电”提示后执行完整外设收尾并进入 ESP32-S3 deep sleep（GPIO5 充电 / GPIO8 ○ 唤醒）。

---

## 2. 分层与所有权

### 2.1 模块图

```
                 ┌────────────────────────────────────────────┐
                 │ main.c：启动顺序，初始化失败不阻塞            │
                 └───────────────┬────────────────────────────┘
                                 ▼
   ┌──────────────────────────────────────────────────────────────┐
   │ mp_app.c   app 任务：状态机、按键、BLE 分发、刷新、恢复        │
   │  运行期唯一调用 mp_store_* 和 display_post() 的模块            │
   │  主循环：抽干 BLE 队列 → 取一个按键 → tick → 有脏才补帧        │
   └───┬──────────┬──────────┬──────────┬──────────┬─────────────┘
       ▼          ▼          ▼          ▼          ▼
   mp_ui.c    mp_page.c   mp_store.c   mp_ble.c   recovery.c
   纯渲染      纯分页      /sdcard/mp   NimBLE      probe/commit
     │                       │           │
     ├─ mp_hints.c           │           └─ 队列 → app 任务
     ├─ mp_font.c            └─ catalog、书、字库、锁图、进度、设置、统计
     └─ gfx.c + text 组件
```

### 2.2 文件所有权

| 文件 | 责任 | 禁止 |
|---|---|---|
| `main.c` | 初始化顺序、启动期 `mp_settings_defaults()` 和 `mp_store_init()`、日志 | 运行期业务、UI、BLE 语义 |
| `mp_app.c` | 屏幕状态、按键、BLE 语义、刷新原语、恢复 | 画像素、直接读文件 |
| `mp_ui.c` | 250×122 渲染、状态栏、列表、阅读、消息、锁屏 | 读 SD、读时钟、碰按键 |
| `mp_hints.c` | 14×14 键帽位图、逐屏底栏表 | 业务状态 |
| `mp_page.c` | 5 行分页、断行、进度、上一页历史 | 绘制、刷屏、BLE |
| `mp_store.c` | 目录、书、字库、锁图、进度、设置、统计 | UI、BLE、显示 |
| `mp_ble.c` | GATT、广播、收包队列、通知、`0xDD` 解析 | `mp_store_*`、业务、日志刷屏 |
| `mp_font.c` | Adafruit GFX 字形适配 | 分页规则 |

### 2.3 线程与队列

- app 任务（`mp_app_run()`，主栈 8192）：所有状态变更、所有 `mp_store_*`、
  所有 `display_post()`；启动阶段配置 CPU DFS (40–240 MHz) 与 Tickless
  Idle，并通过 `ulTaskNotifyTake` + `app_next_wait_ms()` 精确阻塞等待唤醒，
  阅读统计时长改为基于时间戳差分累计。
- NimBLE host 任务：写回调只把包拷进 `mp_ble_req_t` 队列并唤醒 app 任务。
  唯一例外是 `0x42..0x44` 就地回 `[op][GPIO5 电平]`。队列满时按“首条 +
  每 256 条”限频打日志，并回 `ATT INSUFFICIENT_RES`。队列深 48，约为
  原版 web 5832 B 发送窗口的 2 倍。
- input 任务：空闲时切入 GPIO 低电平中断 + `esp_sleep_enable_gpio_wakeup()`
  Light Sleep 硬件唤醒（0 Hz 轮询）；仅在按键活动或去抖期采样，事件队列
  32 条并在入队后回调唤醒 app 任务。
- display 任务：`display_post()` 入队；刷屏期持有 `ESP_PM_NO_LIGHT_SLEEP`
  电源锁；无待办 GC 时以 `portMAX_DELAY` 挂起，有待办 GC 时按精确剩余
  毫秒等待。`display_flush_wait()` 等到队列空且面板 idle。
  `display_get_stats().frames` 只在刷新成功时自增。
- 通知：`mp_ble_notify()` 可在任意任务调用，NimBLE 内部串行化。

主循环（事件驱动，精确计算下一个定时点阻塞等待）：

```c
for (;;) {
    while (mp_ble_req_get(&r, 0)) mp_app_ble_req(&r);  // 1) 抽干 BLE
    while (input_get(&ev, 0)) mp_app_key(...);         // 2) 抽干按键
    // 3) 检查待处理状态、BLE 超时关断、锁屏深睡及按需 tick
    if (s_dirty && !uploading && !debug) { render(); display_post(AUTO); }
    ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(app_next_wait_ms()));
}
```

上传期间 `scene_commit()` 和 `page_commit()` 只置 `s_dirty`，不重绘。
会话结束后主循环补一帧。BLE `0x40` 例外，`debug_draw()` 直接出帧并
`display_flush_wait(2000)`。

---

## 3. 启动顺序

| 步 | 调用 | 失败策略 |
|---|---|---|
| 0 | 日志：版本、构建、IDF、target | 不读 MAC、SN、efuse、NVS |
| 1 | `mp_settings_defaults()` | 无条件先调，保证存储坏时蓝牙仍开、锁屏仍为 3 分钟 |
| 2 | `power_init()` | 记日志继续，电量可能无效 |
| 3 | `storage_init()` | 记日志继续，`storage_ok=0` 时显示 ERROR |
| 4 | `mp_store_init()`，仅卷已挂载时调用 | 失败置 `storage_ok=0` |
| 5 | `display_init()` | 失败继续，`display_ready()==false` 时丢帧 |
| 6 | `input_init()` | 失败继续，按键不可用 |
| 7 | `mp_ble_start()` | 失败继续，广播按 `ble_enabled` |
| 8 | `mp_app_boot(); mp_app_run()` | 永不返回 |

要点：

- `mp_settings_defaults()` 必须在 `mp_store_init()` 和 `mp_ble_start()` 之前。
  否则 SD 坏时 `ble_enabled` 为 0，蓝牙静默关闭，用户既看不到状态，也无法
  用网页恢复。
- `mp_app_state.storage_ok` 在 app 任务启动前由 `main.c` 写入，之后归 app 任务。
- 初始化失败都不致命。按键、BLE 和恢复入口保持可用。
- 启动只打印版本、初始化错误和最终状态，不打印设备身份。

---

## 4. 屏幕与视图模型

### 4.1 视图模型

`mp_ui_state_t` 是 app 与渲染层之间唯一的契约：

- 通用：`screen`、`left/mid/hour/min/time_valid/bt_on/battery/charging`。
- 日期时间：`year/month/day/week`、`time_step`。
- 列表：`row_count/sel/scroll/row[4]`，行带 `kind/value/on/bar`。
- 阅读：`line_count/line_enc/line[5][81]/percent`。
- 关于：`about_lines/about_line[]`。
- 信息：`meta`（用 `\t` 分三段）、`jump_pct`。
- 锁屏：`lock_mode`、`lock_img`、`lock_valid`、`read_mins`、`read_chars`。
- 消息：`msg_lines/msg_line[3]`。
- `hide_hints`：恢复成功页和时间完成页不画底栏。

`mp_ui.c` 不修改 `st`。`mp_ui_render()` 最后画底栏，`hide_hints` 时跳过。

### 4.2 栅格

可见坐标 y=0..121：

```
y=0..17    状态栏：left(8,1)、time(居中)、bt(194,2)、batt(214,2)
y=18       1 px 细分隔线，CLOCK 不画；y=19 留白
y=19..103  内容区
             列表 4×21，行顶 20/41/62/83，反色条加右侧滚动条
             阅读 5×18，行顶 22/40/58/76/94，到 109
             消息按内容块垂直居中，20 px 图标，20 px 行距
             书籍信息 meta@22，点线@41，2×2 按钮 47/75
             时间向导 YYYY-MM-DD @29，HH:MM @66，当前段反色
             跳转 26 px 读数 @52，滑块 @71，刻度 @82
y=104..121 底栏：文字顶 105，键帽 106..119，所有组水平居中
```

例外：

- READ：第 5 行到 109，进度条 `(2,118) 246×2`，没有底栏。
- LOCK 有图：整屏都是图，没有状态栏和底栏。
- RESTORE 和 TIME_DONE 无底栏时按 `y=19..121` 垂直居中。
- RESTORE 不画状态栏时间，6 字标题会给时间让位。

### 4.3 状态栏

- left 从 x=8 起，最大 96 px。UTF-8 走 `mp_ui_text()`，ASCII 用 Inter
  Regular 16，中文用 HZK16。书名走 `mp_ui_gb_text()`。超宽截断，不换行，
  不加省略号。
- time 居中，`x=(250-w)/2`，Inter 16，格式 `HH:MM`。
  CLOCK、JUMP、TIME_SET、RESTORE 的 `time_valid=0`。
- `time_valid=0` 且有 `mid` 时（只有 TIME_SET 的 `1/5`），mid 居中。
- 蓝牙符文在 x=194，12×13。开时显示符文，关时隐藏。
- 电池在 x=214，26×13 描边外框加 2×5 正极帽。有效电量在壳内 22×9 内腔按真实电量比例从左向右填充黑芯，居中 5×7 数字按底色逐像素反色。充电时左侧画 `+`。无效读数只画外框。
- LOCK 不画状态栏。

### 4.4 列表

- 4 行窗口，行顶 `20+21i`。SELECT 和 SETTINGS 的文字从 x=12, y=top+2 起。
- `scroll = mp_ui_list_scroll(sel, row_count)`。装得下不滚，装不下让选中项贴底。
- SELECT 和 SETTINGS 的选中项是 x=6 起、20 px 高的微圆角反色条。
  装不下时右侧 x=243..244 出现滚动条。
- SETTINGS 右侧控件右界为 x=236，有滚动条时为 x=233：
  - `MP_ROW_TOGGLE`：26×12 胶囊开关。开为实心轨道加右圆点，
    关为 1 px 描边轨道加左圆点。
  - `MP_ROW_VALUE`：值右对齐，例如 `3分钟`、`锁屏图`、`11:30`。
  - `MP_ROW_STORAGE`：标签和容量值之间画 6 px 微圆角容量条，不可选中。
  - `MP_ROW_ACTION`：右侧 chevron。
- 设置行顺序：有背光（E2S）8 行（蓝牙、锁屏、锁屏展示、背光、时间设置、存储、关于、恢复原厂固件）；无背光（E2）自动隐藏“背光”共 7 行（存储下标由 5 变为 4）。
  锁屏档位为 1/2/3/5 分钟，默认 3 分钟。光标自动跳过存储行。
  背光档位 0..3（默认 0 关）仅在探测到 AW2023 后有效，由驱动写入 PWM `{0, 5, 20, 35}` 并持久化到 `settings.bin` `buf[9]`；E2 无芯片时完全释放 I2C 总线且长按 `▭` 不弹出快捷浮窗。
  选“锁屏图”但没有有效锁图时，进入 NO_LOCK_IMG 提示页，不改变设置。

### 4.5 阅读区

- 5 行，行顶 `22+18i`，x=2，最大宽 246。
- GB2312 字模顶等于行顶。ASCII 的 cap 顶等于行顶，降部到行顶加 14。
- `mp_ui_read()` 直接使用 `mp_page_t.raw[]`，不加工标点。
- 进度条 `(2,118) 246×2`。已读部分 2 px 实心，空态保留 1 px 底线。
  填充宽 `(246*pct+50)/100`，`pct>0` 时至少 1 px。
- READ 没有底栏。MENU 短按进 BOOK_INFO，长 MENU 或 NEXT 退出阅读。

### 4.6 消息与工具页

- 水平居中：`x=(250-w)/2`。
- 消息按内容块垂直居中。内容块由 20 px 图标、6 px 间距和 20 px 行距组成。
  有底栏时可用区为 19..103，无底栏时为 19..121。
- 图标：翻开书本用于 EMPTY，警告三角用于 ERROR，圆形感叹号用于
  RESTORE 确认、DELETE 和 NO_LOCK_IMG，圆形对勾用于 RESTORE 成功和
  TIME_DONE。
- ABOUT：产品名居中 y=22，点线 y=40，三行键值在 45/64/83。
- BOOK_INFO：状态栏 left 是书名。meta 用 `\t` 分三段，首段左对齐、
  中段居中、末段右对齐。点线 y=41。2×2 圆角按钮 114×22，x=8/128，
  y=47/75。默认选中“跳转”，删除放最后。
- JUMP：`Inter_Num_26` 数字加 `Inter_UI_16` 的 `%`，基线 52。
  轨道在 `(25,71)`，宽 200，带五等分刻度和 11×11 圆形把手。
  刻度标注 `0% / 50% / 100%` 在 y=82。
- TIME_SET：日期 `YYYY - MM - DD` 在 x=25, y=29，时间 `HH : MM`
  在 x=73, y=66。当前字段用 27 px 反色块高亮。
- DELETE：图标加一行 `需重新上传`，长按 OK 才删除。
- TIME_DONE：图标加 `设置完成`，1.2 s 后回 SETTINGS。
- NO_LOCK_IMG：感叹号图标加 `未上传锁屏图` / `用网页上传`。OK 或 MENU
  回 SETTINGS，不改变锁屏展示设置。

### 4.7 锁屏与阅读统计

锁屏有三种呈现：

- 阅读统计（默认，或没有有效锁图）：顶部 12×14 迷你锁头加 `已锁定`。
  上下点线在 y=31 和 y=95，中轴竖点线在 x=125。两栏显示累计
  `阅读分钟` 和 `阅读字数`，数字用 `Inter_Num_26`，单位为 `万`。
  底部居中 `长按[○]解锁`。
- 纯锁屏图：设置选“锁屏图”且 `lock_valid && lock_img` 时显示。
  逐像素映射，不叠文字或白条。源图为 4000 B，`idx=(y>>3)*250+x`，
  `bit(y&7)=1` 为黑，共 128 行。屏幕取源 `y=6..127` 映射到 `0..121`，
  不反相。
- 解锁提示页：纯锁屏图下短按 OK 或按其他键进入。显示 20×22 锁图标、
  `已锁定` 和 `长按[○]解锁`。10 s 无操作后自动回到图片。

长按 OK 在三种状态下都解锁。锁屏进入一律 `scene_commit(force_full=1)`。
解锁后，如果锁前是 READ、BOOK_INFO 或 JUMP，回到 READ 并整屏刷新；
否则回到 CLOCK。

阅读统计写入 `/sdcard/mp/stats.bin`，字段为累计阅读秒数 `read_secs` 和
累计阅读字数 `read_chars`。在 READ 屏每秒加 1 秒，正向翻页时累加刚读完
一页的非空白字符数。每 60 秒写一次，也随进度落盘一起写。

### 4.8 文字管线

| 枚举 | 用途 | 画法 |
|---|---|---|
| `MP_STR_UTF8` | 界面固定文案 | `mp_ui_text()`：ASCII 用 Inter Regular 16，中文用 HZK16 |
| `MP_STR_ASCII` | 英文书名、ASCII 正文 | `mp_text_draw(enc=MP_ENC_ASCII)`，Dialog 比例宽 |
| `MP_STR_GB2312` | 中文书名、GB2312 正文 | `mp_text_draw(enc=MP_ENC_GB2312)`，HZK16 |

- 反色行用 `mp_ui_text_color(..., black=0)` 或
  `mp_ui_gb_text_color(...)`，HZK 和 Inter 都支持白字。
- 阅读正文走 `mp_text_draw()`，按 Miaopaper 的分页规则测量，不用 UI 的 Inter。
- 百分比：READ 只有 2 px 进度条。BOOK_INFO 和 JUMP 用 Inter 的 `42%`，
  不再单独换 Dialog 画 `%`。
- 书名编码判定：标题里出现任意 `>= 0xA1` 的字节就算 GB2312，否则算 ASCII。
  纯 ASCII 书名不能按 GB2312 画，否则每个字母会变全角。
- 阅读正文的 ASCII 基线用 `READER_ASCII_DY=3`。8×16 字形基线落到 HZK16
  的第 15 行，数字墨迹在 top+5..top+14，汉字在 top..top+15。
- 字体许可：Inter 三张表（`Inter_UI_16`、`Inter_Num_26`、
  `Inter_Clock_66`）由 `tools/gen_ui_font.py` 从 SIL OFL 1.1 的 Inter 生成。
  HZK16 只限内部使用，对外发行前必须换成 OFL 字模。

---

## 5. 按键与手势

完整逐屏矩阵在 `docs/KEYMAP.md`。这里只列影响架构的规则：

- 五键只在 CLICK 和 LONG 上动作。DOWN 不产生动作，只取消待发的 OK 单击，
  或记录 `restore_key_ok`。REPEAT 一律忽略。
- 长按 1.0 s（`READER_KEY_LONG_MS=1000`）。
- OK 的 CLICK 在所有屏延迟 350 ms（`MP_DCLICK_MS`）。350 ms 内第二次
  CLICK 算双击：READ 上回上一页，其他屏无动作并吞掉这对单击。
- 任何非 OK 键、任何切屏、OK LONG 都取消待发的 OK 单击。BLE `0x21`
  切回 CLOCK 时也清 `pending_ok`。
- OK 长按在 CLOCK、SELECT、READ、SETTINGS、ABOUT、EMPTY、ERROR、
  BOOK_INFO 上锁屏。RESTORE 长按提交，DELETE 长按删除。
  JUMP、TIME_SET、TIME_DONE 不把长按当锁屏。
- 锁屏时短按 OK 只显示解锁提示，长按 OK 解锁。
- READ 短 MENU 打开 BOOK_INFO，长 MENU 或 NEXT 保存进度并退出阅读。
- DEBUG（`0x40`）只响应 MENU 长按、`0x41` 和 30 s 超时。

---

## 6. 刷新、GC 与 sleep 降级

### 6.1 display 的真实行为

- 5 s 最小间隔只约束“局刷之后的全刷”。条件为
  `gap_ok = !attempted || last_was_full || since >= 5000`。不满足时
  display 任务先 `vTaskDelay`，最长约 5 s，然后才画。
- `display_post(fb, FULL)` 不保证立刻出帧。`AUTO` 走局刷分支，通常立即出帧。
- `display_post()` 在 `!s_ready` 时直接丢帧。`display_flush_wait()` 没有
  返回值，只在队列空且面板 idle 时返回。
- `frames` 只在刷新成功时自增，不能证明“这一帧”已经上屏。只有 RESTORE
  用 `frames` 判断画面是否真的更新。

### 6.2 两个刷新原语

```c
/* 场景切换：先出帧，等这一帧上屏，再可选补全刷。 */
scene_commit(force_full):
    if (!display_ready()) return;
    if (uploading) { s_dirty=1; return; }
    render(); display_post(AUTO);
    display_flush_wait(2000);
    if (force_full || !mp_settings.epd_partial_enabled)
        display_request_full();

/* 屏内动作：翻页、移光标、改值，只局刷，不等。 */
page_commit():
    if (!display_ready() || debug_mode) return;
    if (uploading) { s_dirty=1; return; }
    render(); display_post(AUTO);
```

不直接用 `display_post(FULL)` 当全刷，是因为它会被 5 s 间隔挡住，屏幕停在
旧内容 4..5 s。先 `flush_wait` 再 `request_full` 时，flush 后
`s_dirty==false`，`request_full()` 落在“无新帧 + force”分支，重画同一帧，
不会等 5 s 才出内容。

### 6.3 决策表

| 场景 | 调用 | 说明 |
|---|---|---|
| 场景切换 | `scene_commit(0)` | `epd_partial_enabled` 时不补全刷，默认补一次 |
| 翻页、移光标、改值 | `page_commit()` | 局刷；基线无效时 display 自动升全刷 |
| 锁屏 | `scene_commit(1)` | 不看 `epd_partial_enabled`，一律补全刷 |
| 错误页 | `scene_commit(1)` | 同上 |
| 手动全刷（FUNC 短按） | `display_request_full()` | UI 不阻塞、不等待 |
| 恢复确认页和成功页 | `post(AUTO) → flush_wait(8000) → frames → request_full()` | 只在 RESTORE |
| 上传中 | 不刷新 | 只置 `s_dirty`，会话结束补帧 |

禁止每页全刷，禁止把 `display_post(FULL)` 当立即全刷，也不使用
`DISPLAY_MODE_PARTIAL`，因为它等价于 AUTO。

### 6.4 GC

- `display_set_gc(partials, READER_GC_IDLE_MS)`。`partials` 取
  `mp_gc_interval[gc_interval_idx] = {10, 20, 50, 100}`，空闲 GC 为 45 s。
- 开机必须应用一次有效设置，否则会使用 `reader_cfg.h` 的默认值 16。
- `epd_partial_enabled` 和 `gc_interval_idx` 仍由设置结构维护，BLE `0x1D`
  和 `0x1E` 可改，但设置页不再暴露“快刷/全刷”。改完立即
  `display_set_gc()`。
- OED 面板（`family != 1`）每页本来就会全刷。内部开关只影响场景进入的
  意图，不承诺更快。

### 6.5 锁屏、深度休眠与低电关机

日常超时降级为锁屏（有背光时同时调用 `mp_backlight_suspend()` 关闭背光省电，解锁后恢复原亮度；无背光 E2 跳过）：

| 参数 | 值 |
|---|---|
| 超时档位 | 60 / 120 / 180 / 300 s，默认 180 s |
| 到点动作 | `lock_enter()`：保存进度、背光待机、载入锁图、`scene_commit(force_full=1)` |
| 不自动锁 | BLE 已连接、上传中、DEBUG、LOCK、RESTORE、BOOK_INFO、JUMP、DELETE、TIME_SET、TIME_DONE、BACKLIGHT、LOW_BATT |
| 锁屏自动关蓝牙 | 锁屏后持续 120 s（2 分钟）无 BLE 活动自动调用 `mp_ble_stop()` 关断射频 |
| 锁屏深度休眠 | 锁屏且蓝牙关闭后宽限 10 s（锁屏内刚自动关蓝牙宽限 2 s）自动进入 Deep Sleep |
| 解锁与深睡唤醒 | 未深睡前长按 OK 解锁；已深睡时需长按 GPIO8 `○` 满 500 ms 恢复原阅读页或时钟页，短按误触立刻原样回深睡 |
| 唤醒后状态 | 恢复背光及 POSIX RTC / `RTC_DATA_ATTR` 中的时间与阅读现场；锁前 READ、BOOK_INFO 或 JUMP 时回 READ 并整屏刷新，否则回 CLOCK |

锁屏自动深度休眠流程（保留当前锁屏画面且不重新刷屏）：

1. 若按键 GPIO8 仍未松开，等待其松开（上限 2000 ms + 100 ms）；
2. 将当前阅读位置、阅读统计及 RTC 上下文写入 `RTC_DATA_ATTR` 并落盘持久化；
3. `display_sleep()`（保留 EPD 画面）→ `mp_backlight_suspend()` → `storage_unmount()`（拉高 GPIO34 断电）→ 锁存深睡期引脚状态（GPIO34/45）；
4. 仅配置 GPIO8 `○` EXT1 低电平唤醒后调用 `esp_deep_sleep_start()`。下一次唤醒由 `app_main()` 最早期的 `mp_app_early_wake_check()` 校验 500 ms 长按防误触。

低电自动深睡关机（未充电且电量 `< 3%`，开机门限与运行期复采确认双触发）：

1. 中止上传并调用 `mp_app_flush_progress()` 落盘进度与阅读统计；
2. 全刷 `MP_SCREEN_LOW_BATT`（`52×26` 感叹号空电池 + `电量耗尽` / `请充电`）并 `display_flush_wait(8000)`；
3. `display_sleep()`（BOE `0x10 0x01` / OED `0x07 0xA5` 保留画面）→ `mp_backlight_suspend()` → `storage_unmount()`（拉高 GPIO34 断电）→ GPIO45 拉低 → 延时 1 s；
4. 配置 EXT1 低电平唤醒（GPIO5 充电插入 | GPIO8 ○ 确认键，RTC 内部上拉）后 `esp_deep_sleep_start()`。

### 6.6 上传、锁屏与刷新互斥

- `uploading=1` 时，`scene_commit()`、`page_commit()` 和主循环的 `s_dirty`
  补帧都不重绘。BLE `0x40` 例外，它用 `debug_draw()` 直接出帧并 flush。
  上传期间不采样电量，ADC 会阻塞至少 120 ms；也不自动锁屏。
- 锁屏进入前如果有上传会话，先 `upload_abort()` 回滚半本书。
- 恢复确认页在 8 s flush 期间抽干 input 队列，再 armed，避免排队的长按
  直接提交 otadata。

---

## 7. 分页与阅读

### 7.1 冻结常量

| 量 | 值 | 来源 |
|---|---|---|
| 每页行数 | 5 | `MP_READ_LINES` |
| 每行读取上限 | 80 B | `MP_READ_BUF` |
| 行宽上限 | 246 px | `MP_MAX_LINE_W` |
| 行距 | 18 px | `MP_LINE_H`，仅渲染，不影响分页 |
| ASCII 越界字节宽 | 6 px，空格宽 | `MP_ASCII_MIN_W` |
| 上一页窗口 | 400 B + 80 B | `MP_PAGE_BACK_MAX/WIN_BYTES` |
| 页首历史 | 16 深 | `MP_PAGE_HIST_DEPTH` |

### 7.2 断行规则

- ASCII：使用 Dialog 字体的 `xAdvance`。`\n` 立即断并包含该字节，`\r` 跳过。
  超宽时退到最后一个空格之后，没有空格则硬断。`<0x20` 或 `>0x7E` 的字节
  先替换成 6 px 空格宽，并参与“最后空格”回退。
- GB2312：`hi>=0xA1 && lo>=0xA1` 的双字节按 16 px。其余单字节也按 16 px，
  即 A3 全角 ASCII。超宽即断，没有回退。
- 目标是与 Miaopaper 的 `find_line_break_ascii()` 和
  `find_line_break_gb2312()` 逐字节一致。越界字节不能写成 8 px。

### 7.3 分页、上一页、进度

- `mp_page_measure()` 每页 5 行，每行只读 `min(80, len-pos)`。它填充
  `line_off/line_len/line_w/raw`，`raw` 以 NUL 结尾。读失败时 `lines=0`，
  调用方显示错误页。
- 上一页使用 16 深 LIFO 历史。没有历史时 `mp_page_prev_start()` 反推：
  `S==0` 返回 0；在 `[lo,S)` 中从小到大找“页尾等于 S”的精确匹配；
  无匹配时退化为“包含 S-1 的页”。
- 进度：`mp_page_percent(pos,len)` 用 64 位四舍五入并夹到 0..100。
  READ 页在 `at_end` 时直接显示 100%。
- 渲染：`mp_ui_read()` 使用 `raw[]`，基线为行顶加 12，不加工标点。

### 7.4 验证边界

`sim/` 编译 `mp_page.c`。READ 三帧由真实 `mp_page_measure()` 从连排书生成。
`--check` 断言行宽和末页，并比对 25 帧像素哈希。逐页 `[start,next)` 与
Miaopaper 的 15,765 点对拍是开发期夹具，不在 `--check` 内重跑。

---

## 8. 存储

### 8.1 设备与目录

- 设备是内置 SD NAND（SDMMC 4-bit），挂载到 `/sdcard`，没有卡槽。
- `mp_store` 只写 `/sdcard/mp`。启动期共享 `storage_init()` 会创建
  `/sdcard/books`、`/sdcard/reader`、`/sdcard/reader/state` 和
  `/sdcard/reader/tmp`，并扫描 `/sdcard/books`。所以“绝不碰
  /sdcard/books、/sdcard/reader”不成立。mp 不写其中的书或进度内容。
- 不格式化 SD。`/sdcard/bk` 和 `/sdcard/cache` 不在创建或扫描路径里。

| 文件 | 大小或格式 | 作用 |
|---|---|---|
| `catalog.bin` | 264 B，8 B 头加 8×32 B 条目 | 8 槽目录 |
| `catalog.tmp/.bak/.bad` | 264 B | 原子替换、回滚、损坏现场 |
| `b0.bin..b7.bin` | 原始字节 | 每槽一本书 |
| `font.bin` | 261,696 B | 上传的 HZK16，不参与渲染，只为 `0x16..0x19` 兼容 |
| `lock.bin` | 4096 B，4000 B 图加 `0xAA` @0x0FF0 | 锁图，无标志表示没有图 |
| `lock.tmp/.bak` | 4096 B | 上传中间文件和旧图 |
| `progress.bin` | 8×8 B | 每槽定长进度，不是追加环 |
| `settings.bin` | 16 B | `MPS1` 加 ble、sleep、partial、gc、lock_display、backlight_level 和 XOR |
| `stats.bin` | 16 B | 累计阅读秒数和累计阅读字数 |

### 8.2 catalog 与槽状态

头为 `BOOK`、ver=1、`font_inst`、`book_cnt=0`、保留 0。

条目 32 B：

| 偏移 | 长 | 字段 |
|---|---|---|
| 0x00 | 1 | flags |
| 0x01 | 20 | title，最多 19 B，第 20 B 为 0 |
| 0x15 | 4 | start，恒 0，SD 上一书一文件 |
| 0x19 | 4 | len，声明长度，LE32 |
| 0x1D | 1 | enc，0=ASCII，1=GB2312 |
| 0x1E | 2 | chars_k，上传时统计的字数除以 1024，LE16 |

状态机：

```
EMPTY 0xFF --begin--> UPLOADING 0xFE --finish(长度和文件校验通过)--> DONE 0xFC
   ^                         |
   +---- abort 或掉电回收 ----+
DONE --delete--> DELETED 0x00，条目清零；compact 时转回 EMPTY 0xFF
```

`find_free_slot()` 取第一个非 DONE 且非当前写槽的槽。`0x14` 只数 DONE，
`0x15` 按槽号查。`chars_k=0` 表示旧目录项没有字数统计。

### 8.3 原子替换与损坏恢复

`flush_catalog_locked()` 的顺序：

1. 写 `catalog.tmp` 并 `fsync`。
2. 旧 `catalog.bin` 改名为 `catalog.bak`。
3. `rename(tmp, catalog.bin)`，再 `fsync` 目录。

`load_catalog_locked()` 的处理：

- 文件不存在时，从 `.tmp` 或 `.bak` 恢复。都失败则重建 8 个空槽。
- magic 或长度不对时，改名为 `catalog.bad` 保留现场，再尝试从 `.tmp`
  或 `.bak` 恢复。都失败才重建。
- 只要 `load_catalog_locked()` 报告损坏，包括恢复或重建成功，
  `mp_store_init()` 都返回 -1，app 显示 ERROR 页。下一次 `[↻]重试`
  通常会恢复为 0。

### 8.4 写入与打开校验

- `begin` 记录 `declared_len`。`write` 拒绝 `off > declared`、
  `len > declared-off` 和 `off != written`。
- `finish` 要求 `written == declared`、`fstat().st_size == declared` 且
  `fsync` 成功，才置 DONE。目录落盘失败会回滚为 EMPTY。
- 打开书时 `mp_store_book_size()` 返回 `min(declared, st_size)`。
  `st_size < 16` 视为坏书，打不开。单本坏书仍显示存储错误页，见 L1。

### 8.5 进度、设置、统计与容量

- 进度：翻页只写 RAM。长 MENU、长 NEXT、`0x20`、`0x21`、锁屏、空闲 2 s
  和恢复提交前才写 `progress.bin`。
- 设置：16 B 加 XOR。默认 `ble=1`、`sleep=2`（180 s）、`partial=0`、
  `gc=0`（10）、`lock_display=阅读数据`。存储失败时
  `mp_settings_defaults()` 仍在 RAM 中生效。
- 统计：`stats.bin` 保存累计阅读秒数和字数。READ 屏每秒加 1 秒，
  正向翻页累加字符数。每 60 秒写一次，也随进度保存。没有清零入口。
- 容量：`esp_vfs_fat_info("/sdcard")`，按 1024²（MiB）换算，四舍五入到 0.1 MB，在关于页展示。
  失败时显示 `不可用`。
- `mp_store` 只在 app 任务调用。所有 `/sdcard/mp` 路径共用同一把递归锁。

---

## 9. BLE 传输层

完整请求和应答表在 `docs/PROTOCOL.md`。这里只记架构选择：

- 不用 `../e2s_reader/components/ble`，协议完全不同。直接使用 NimBLE 实现
  `0x221F/0x331F` 等 16-bit 服务，`REQUIRES bt`，不加 `ble`。
- 广播名固定 `MPP_E2S`。Service Data 的 MAC6 全 0，不广播或打印 MAC、SN、
  efuse、NVS。
- 自动省电与彻底关断：
  - 按需启动：仅设置项 `ble_enabled` 为开时才启动 NimBLE 协议栈；运行期开关同步调用
    `mp_ble_start()` / `mp_ble_stop()`（停止广播、断开连接、`nimble_port_stop()` +
    `nimble_port_deinit()` 彻底关断 BT 控制器与射频）。
  - 双条件自动关断：锁屏后连续 120 s（2 分钟）无 BLE 通信自动关闭蓝牙；亮屏/日常连续
    600 s（10 分钟）未使用蓝牙自动关闭蓝牙。
  - 连接功耗优化：启用 `CONFIG_BT_CTRL_MODEM_SLEEP` 与 +3 dBm 发射功率；上传阶段持有
    `ESP_PM_NO_LIGHT_SLEEP` 锁，传输完成后切回空闲连接参数（50–75 ms, latency 4）。
- `No_Security`，不配对不加密，锁屏必须自己做应用层白名单。
- MTU：preferred 247。`ROLE_CENTRAL` 和 `GATT_CLIENT` 打开后，设备在连接时
  主动 `ble_gattc_exchange_mtu()`；上传开始若 MTU < 100 再补一次。
  notify 强制 `len ≤ min(244, MTU-3)`，超长失败而不是截断。
- 队列：写回调只拷贝 `mp_ble_req_t`。队列满时 `dropped++` 并回
  `ATT INSUFFICIENT_RES`。1 MB 上传的目标是 `dropped==0`，尚未上真机。
- 会话：同一时刻只允许一个上传，类型为 book、font 或 lock。60 s 无数据
  自动 abort。断连时 `xQueueReset` 并 `conn_cb(0)`，上一会话的残留包不会
  进入下一会话。
- 锁屏白名单：蓝牙尚未超时关断前，只读或设置 6 个 op 加 `0x21`，`0x21` 只 flush。
  上传、切屏、调试、删除全部拒绝。`0x42..0x44` 和 `0xDD` 在传输层就地处理，不经过
  白名单。锁屏后若连续 120 s 无 BLE 通信则自动调用 `mp_ble_stop()` 关断射频。

---

## 10. 恢复原厂

### 10.1 边界

- 只有 `recovery_stock_commit()` 能写 otadata。`recovery_stock_probe()`
  只读校验。
- 不接收、不下载、不写任何 app 镜像，不做任意槽切换。
- 目标槽必须有通过 `esp_image_verify()` 且 `project_name=="paperang_e2"` 的
  原厂镜像。secure boot、flash encryption、anti-rollback 或调试器连接任一
  成立时返回 `RECOVERY_ERR_UNSUPPORTED`，不做绕过。
- 只擦写 otadata 中不持有最高可用 seq 的 4 KiB 扇区，保留一条有效记录
  作回退。

### 10.2 入口与确认

- 入口只有 SETTINGS 第 8 项 `恢复原厂固件`，短按 OK 进入。
- `display_ready()==false` 时静默拒绝，只写日志，不画页，不写 flash。
- probe 失败时显示恢复失败页，不写 flash。
- 确认页流程：`post(AUTO) → flush_wait(8000) → frames 必须增加 →
  request_full()`。然后抽干 input 队列，置 `restore_armed=1`。
- armed 之后必须见到新的 OK DOWN（`restore_key_ok`）才接受 OK LONG。
  这可以防止 8 s 黑屏期排队的长按直接提交。

### 10.3 提交与结果

```
flush_progress()
recovery_stock_commit()
  OK   → 成功页（hide_hints=1）→ post + flush(8000) → 3 s → esp_restart()
  失败 → 失败页，不重启，restore_armed=0
```

- 成功页文案为 `已切换` / `3秒后重启`，两行在 44/62，无底栏。
- 失败文案：`原厂固件不存在`、`分区表不匹配`、`未开放此功能`、
  `写入失败`、`校验失败`。面板就绪但帧没有上屏时显示
  `刷新失败 / 画面停在上一帧 / 重启设备`。
- 切到原厂是单向操作。要回 mp，只能在原厂运行时把 mp 烧到当前活动槽。
  写另一槽不改变启动选择。界面不承诺能回来。
- 所有离开 RESTORE 的路径，包括 MENU 取消、失败、切屏和 BLE `0x21`，
  都必须清 `restore_armed` 和 `restore_key_ok`。RESTORE 屏禁止自动锁屏。

---

## 11. 已知限制与偏差

### 11.1 仍存在的限制

| 编号 | 级别 | 影响 | 证据 |
|---|---|---|---|
| L1 | medium | 单本坏书（文件缺失、`st_size<16`、打开后读失败）显示“存储错误/读不到内置存储”，重试只重挂载。打不开的坏书没有 UI 删除入口。截短到 ≥16 B 时按真实大小打开，内容变短但不报错 | `mp_store.c`、`mp_app.c` |
| L6 | 设计 | 分钟变化只重绘 CLOCK。其他屏的状态栏时间停在最近一次重绘，避免阅读中每分钟整屏刷新 | `mp_app.c`、`mp_ui.c` |
| L10 | 设计 | 锁屏先停蓝牙再进 Deep Sleep；锁屏无通信 2 分钟或日常空闲 10 分钟自动 `mp_ble_stop()` 关断射频 | `sdkconfig.defaults`、`mp_ble.c`、`mp_app.c` |
| L12 | low/doc | 共享 `storage_init()` 会创建并扫描 `/sdcard/books` 和 `/sdcard/reader/{state,tmp}`。`mp_store` 不写这些路径，但“绝不碰”不成立 | `../e2s_reader/components/storage/storage_sd.c` |

补充：

- 阅读统计只累计，没有清零入口。
- 2026-11-25 第二轮修复了锁图完整性、lock/font/progress/settings 递归锁、
  恢复成功页行数、ASCII 基线、ERROR 文案、`0x21` 与 `0x2A19/0x2A1F`
  通知、模拟器哈希。详细状态见 `docs/BUILD.md`。

### 11.2 与 PORT_SPEC 的实现偏差

| 规范 | 代码 | 说明 |
|---|---|---|
| `mp_store_space()` 公开 API | `mp_app.c` 的 `store_space()` 直接调用 `esp_vfs_fat_info("/sdcard")` | 行为一致，ABOUT 和 `0x10` 的 `0x02` 预检共用该函数 |
| `mp_err_t` 错误枚举 | 仍以 `0/-1 + mp_store_error()` 为主 | BLE 状态码在 app 层映射，用户可见行为不变 |
| 删除后条目回 `EMPTY 0xFF` | 实际写 `DELETED 0x00` | `find_free_slot()` 和 `0x14` 行为不变，仅目录字节格式不同 |
| 锁屏下一直维持 BLE 广播 | 双条件自动关闭 + 锁屏无蓝牙深睡 | 锁屏 2 分钟无 BLE 通信或日常空闲 10 分钟自动 `mp_ble_stop()` 关断射频，锁屏且无蓝牙宽限后进 Deep Sleep |
| `MP_DIR` 可被 `-D` 覆盖 | `mp_store.c` 直接 `#define` | sim 不编译 `mp_store.c`；主机测存储前需改成 `#ifndef` |
| 只写 `/sdcard/mp` | `mp_store` 只写 `/sdcard/mp`，共享 `storage_init()` 会创建并扫描平台目录 | 不写书或进度内容，目录副作用见 L12 |

### 11.3 评审快照中已修复的项

07:xx 评审快照中的问题已在当前代码中修复，不再作为限制：

- `0x10` 失败应答统一为 `[0x10][st][0xFF]`。
- catalog 从 `unlink` 重建改为 tmp、bak、rename 加 `catalog.bad`。
- `0x13` 删到最后一本时进 EMPTY，`sel` 越界时夹紧。
- 上传成功后 EMPTY 屏会切到 SELECT。
- `book_size` 使用 `min(declared, st_size)`，`finish` 校验 `fstat` 和 `fsync`。
- `0x32` 后锁图缓存会重载。
- 设备可以主动发起 MTU 交换。
- `0x33` 在 MTU<166 时回 `[0x33][0xFE]`。
- 锁屏白名单改为白名单。
- `tools/flash_app.sh` 已存在。
- `mp_font.c` 的位连续字体按 bit 推进，不再按 stride 读。

---

## 12. 验证状态

运行结果见 `docs/BUILD.md`。设计层面需要说明：

本次实际运行：

- ESP-IDF 构建：`Project build complete`，镜像 936,800 B（0xe4b60）。
- `sim/run.sh --check`：27 帧，0 失败，文案 lint 通过。
- `sim/run.sh --all`：27 帧 PNG 和 contact sheet。
- `tools/flash_app.sh` 只打印，`tools/build.sh flash` 退出 3。

仅静态复核：

- catalog 原子替换、坏书路径、锁图完整性、恢复的 armed 门、锁屏白名单。

完全未验证：

- 真机 BLE、上传、MTU、`0x33`、`0xDD`。
- 面板刷新、GC、残影。
- 恢复 commit 和 `esp_restart()`。
- 掉电注入、电流。
