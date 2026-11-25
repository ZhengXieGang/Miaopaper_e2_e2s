# Miaopaper 行为与协议施工契约（E2S 移植基线）

- 参考实现：`refs/Miaopaper`（原始分析工作区路径，参考仓库不随本仓库分发），git HEAD `1ef51c01778651030b012840149e84e55307cdc9`（`git rev-parse HEAD`；`git log -1` 日期 2026-08-27，标题 `ebook: unlock/scene changes always full-refresh`）。
- 证据约定：`Firmware/src/xxx.c:NN` 指参考仓库内该文件行号；网页工具统一记作 `web(MiaoPaper.html):NN`（仓库根目录的 HTML 控制台）。本文所有行号均来自本次会话直接读取的文件内容。
- 方法：**只读源码**。本次没有构建、没有运行固件、没有打开网页工具、没有刷机，也没有执行参考仓库内的任何脚本。因此下文所有"行为"都是静态阅读结论，不是实测。
- 定位：本文是**行为/协议契约**，不是视觉稿。像素坐标只作为"结构证据"记录（说明元素存在与占位），E2S 允许换配色/字体/图标；但凡是主机可见的字节格式、状态迁移、页边界算法，都按"必须一致"对待。
- 与任务书的差异（先声明，后文展开）：
  1. 阅读正文行高不是统一 18 px：**GB2312 = 18 px，ASCII = 19 px**（`ebook.h:92-93`）。
  2. 目录项里**标题字段 20 字节，但写入方最多写 19 字节**（`ebook.h:38`、`ebook.c:107`、`ebook.c:432`）。
  3. "删除"并不写 `DELETED(0x00)`：全仓库只有定义没有写入，删除是**整扇区重写并丢弃该槽**，槽回到 `EMPTY(0xFF)`（`ebook.h:48-51`、`ebook.c:244-278`；本次执行 `grep -rn "EB_FLAG_DELETED" Firmware/src/*.c Firmware/src/*.h`，只命中 `ebook.h:51` 一处定义）。

---

## 1. 模式状态机

### 1.1 状态与全局变量

| 符号 | 定义 | 说明 |
|---|---|---|
| `eb_mode_t` | `CLOCK=0, SELECT, READ, LOCK, SETTINGS, ABOUT, DEBUG` | `ebook.h:103-111`（DEBUG 是 BLE 分辨率调试用，按键不可达） |
| `eb_mode` | `RAM`，初值 `EB_MODE_CLOCK` | `ebook.c:37` |
| `eb_prev_mode` | `RAM`，初值 `EB_MODE_CLOCK` | `ebook.c:41`，只在进 LOCK 时写入 |
| `eb_selected` | SELECT 光标 | `ebook.c:38` |
| `eb_set_selected` | SETTINGS 光标（文件内 static） | `ebook.c:1191` |
| `ebook_state` | `active/book_idx/char_pos/prev_char_pos/book_start/book_len/encoding` | `ebook.h:113-121`、`ebook.c:35` |
| `render_pending` | static，渲染被 EPD 忙阻塞时置位 | `ebook.c:45` |

上电**永远落在 CLOCK**，不自动续读；只把上次的书号/位置载回 `ebook_state` 并把 `settings.ebook_active` 清 0（`ebook.c:525-549`，关键 `ebook.c:538-548`）。

### 1.2 迁移表（逐条可查）

| 起点 | 触发 | 终点 | 证据 |
|---|---|---|---|
| —（上电） | `ebook_init()` | CLOCK | `ebook.c:548` |
| CLOCK | L 单击 | SETTINGS | `ebook_buttons.c:83` → `ebook.c:1257-1264` |
| CLOCK | L 长按(1 s) | SETTINGS | `ebook_buttons.c:134-135` |
| CLOCK | R 单击 | SELECT | `ebook_buttons.c:92` → `ebook.c:1108-1116`（内部有 `eb_mode != CLOCK` 守卫） |
| CLOCK | R 长按 | SELECT | `ebook_buttons.c:120-122` → `ebook.c:1120-1124` |
| CLOCK | F 长按 | LOCK | `ebook_buttons.c:123-129` → `ebook.c:978-1004` |
| CLOCK | 空闲超时且 BLE 未连接 | LOCK（自动锁屏） | `app.c:190-208` |
| SELECT | F 单击且光标 < 书数 | READ | `ebook.c:1137-1144` → `ebook_open()` `ebook.c:551-588` |
| SELECT | F 单击且光标 == 书数（"Back to Clock"） | CLOCK | `ebook.c:1145-1150` |
| SELECT | R 长按 | CLOCK | `ebook.c:1125-1129` |
| SELECT | L/R 单击 | 光标 ±1，越界环绕 | `ebook.c:1153-1183` |
| READ | R 长按 | CLOCK（保存进度） | `ebook.c:1130-1133` → `ebook.c:599-612` |
| READ | F 单击 / R 单击 | 下一页 | `ebook_buttons.c:64`、`ebook_buttons.c:91` |
| READ | L 单击 / F 双击 | 上一页 | `ebook_buttons.c:82`、`ebook_buttons.c:67-70` |
| SETTINGS | L 长按 | CLOCK | `ebook_buttons.c:136-137` → `ebook.c:1266-1272` |
| SETTINGS | 光标在 Exit 且 F 单击 | CLOCK | `ebook.c:1331-1332` |
| SETTINGS | 光标在 About 且 F 单击 | ABOUT | `ebook.c:1328-1329` → `ebook.c:1426-1432` |
| SETTINGS | L/R 单击 | 光标 ±1，越界环绕 | `ebook.c:1274-1300` |
| ABOUT | F 单击 | SETTINGS | `ebook.c:63` → `ebook.c:1434-1439` |
| ABOUT | L/R 单击 | 两页切换（`about_page` 0↔1） | `ebook.c:1413-1424` |
| LOCK | F 单击**或**F 长按 | 解锁 → 见 §1.3 | `ebook_buttons.c:60`、`ebook_buttons.c:125-127` |
| 任意（含 DEBUG；LOCK 下改为解锁） | F 长按 | LOCK / 解锁 | `ebook_buttons.c:123-129`；`ebook_handle_lock` 无模式守卫 |
| DEBUG | BLE `0x40` / `0x41` | 进入 / 回 CLOCK | `ebook_ble.c:730`、`ebook_ble.c:751-752` |

### 1.3 进/出条件（规范性）

**CLOCK**
- 进入：上电、SELECT 的 "Back to Clock"、SELECT 长按 R、READ 长按 R、SETTINGS 退出、LOCK 解锁后的兜底分支。
- 退出：`L`（短/长）→ SETTINGS；`R`（短/长）→ SELECT；`F` 长按 → LOCK；空闲超时 → LOCK。
- 主循环在 CLOCK 下每轮都调 `epd_update()`，但真正重绘被 `update_time_scene()` 的"分钟变化"闸门限制（`app.c:97-104`、`epd.c:393-419`）。

**SELECT**
- 进入有守卫：`if (eb_mode != EB_MODE_CLOCK) return;`（`ebook.c:1110`）。这是六个模式里唯一显式守卫的入口（SETTINGS/ABOUT 的入口函数没有守卫，只靠调用点保证）。
- 列表内容 = `DONE` 槽按槽号升序 + 末尾一条合成项 "Back to Clock"；总数 = 书数 + 1（`ebook.c:1051-1067`）。
- 可见 4 行，`scroll = clamp(sel, 0, total-4)`（`ebook.c:1066-1072`）。

**READ**
- 进入：`ebook_open(idx)`（`0x20` 或选书确认）。`ebook_open` 会：读目录 → 置 `active=1` → `char_pos` 从进度环恢复 → `eb_hist_reset()` → 落盘 settings → 立即绘制（`ebook.c:551-588`）。
- 退出：R 长按（保存并回 CLOCK）；F 长按（LOCK，`ebook_state` 保持 active）；BLE `0x21`（`ebook_close()`，回 CLOCK 并强制全刷时钟，`ebook.c:590-596`）。
- READ 内**没有二级菜单**。

**SETTINGS**
- 进入：CLOCK 下 L 短按或 L 长按；ABOUT 按 F 返回。
- 退出：L 长按；光标在 Exit 按 F；F 长按 → LOCK。
- 6 项，可见 4 行，`scroll = clamp(sel, 0, 6-4)`（`ebook.c:1190`、`ebook.c:1209-1212`）。

**ABOUT**
- 进入：SETTINGS 第 5 项（index 4）按 F。两页：page0 = 产品名 + FW 版本 + 构建日期/时间；page1 = 作者/邮箱/B站/GitHub + 头像位图（`ebook.c:1355-1410`）。头像用 4000 B 覆盖缓冲 `avatar_obd_buf` 直接 OR 进 `epd_temp`（`ebook.c:1349-1353`）。

**LOCK**
- 进入：任意界面 F 长按；或空闲超时（且 BLE 未连接）。`ebook_handle_lock()` 先 `eb_prev_mode = eb_mode`，再 `eb_mode = LOCK`，并**清 `epd_partial_ready`**（因为锁屏是场景切换，必须全刷）（`ebook.c:978-993`）。
- 退出：`ebook_handle_unlock()`：`eb_mode = eb_prev_mode`，若为 `SETTINGS/SELECT/ABOUT` 则改回 CLOCK；若为 READ 则清 `epd_partial_ready`、置 `render_pending`、重画当前页（全刷）；其余一律 CLOCK 并 `set_EPD_wait_flush()` + `epd_update()`（`ebook.c:1006-1037`）。
- 锁屏渲染：见 §2.7。

**DEBUG**
- 只由 BLE `0x40` 进入、`0x41` 退出；`eb_mode` 直接赋值，不经 `eb_prev_mode`（`ebook_ble.c:730`、`ebook_ble.c:751`）。按键在 DEBUG 下全部无操作（所有 handler 都没有 DEBUG 分支）。

### 1.4 锁屏恢复（`eb_prev_mode`）的精确语义

- `eb_prev_mode` **只在** `ebook_handle_lock()` 里写（`ebook.c:983`）。自动锁屏走同一个函数（`app.c:202`），所以自动锁屏也能恢复到阅读页。
- 解锁时 READ 能恢复是因为 `ebook_state` 在锁屏期间没有被销毁；重新进入 READ **不做** `ebook_open()`，只是重画 `char_pos` 处（`ebook.c:1023-1029`）。因此**锁屏前未保存的翻页位置不会丢**（每次翻页本来就落盘）。
- 菜单类（SETTINGS/SELECT/ABOUT）解锁一律回 CLOCK（`ebook.c:1013-1015`），理由是"用户不期望锁屏后落在设置页"（注释 `ebook.c:1009-1011`）。
- LOCK 状态下不响应 L/R（无 handler），F 双击也**不解锁**：双击走 `on_dblclick_front()`，该函数只在 READ 下动作（`ebook_buttons.c:67-70`、`ebook_buttons.c:155-158`）。这是可复刻的既有行为，也是 UX 陷阱。

### 1.5 渲染互斥（`render_pending`）

所有绘制入口先检查两件事：`ext_flash_is_safe()`（外挂 flash 与 EPD 共用 CLK/MOSI，等价于 `epd_update_state == 0`，见 `ext_flash.c:8-11`）与 `epd_update_state`；忙则 `render_pending = 1` 直接返回，由主循环 `ebook_check_pending_render()` 补画（`ebook.c:853-857`、`ebook.c:941-951`、`ebook.c:1041-1044`、`ebook.c:1443-1462`、`app.c:112`）。

E2S 的 SD 与 EPD 不共享总线，可以退化为"渲染忙标志"，但**"分页测量不碰存储"**的设计必须保留（见 §2.4）。

---

## 2. 屏幕绘制契约（结构，不含视觉细节）

### 2.1 画布与坐标系

| 项 | 值 | 证据 |
|---|---|---|
| 逻辑画布 | 250 × 128，1 bpp，`epd_temp` 为 OBD 虚拟屏 | `ebook.h:57-58`、`ebook.c:859-860` |
| 存储格式 | `byte = (y>>3)*250 + x`，`bit = 1<<(y&7)`，**bit=1 为黑** | `ebook.c:650-657`、`web:592-604` |
| 面板可见区 | 只有 y≥6 可见，y≈122 以下裁掉；实现按"128 行、上 6 行不可见"处理 | `epd.c:456-465`（注释）、`epd.c:332`（"122 real pixel, but needed to have a full byte"） |
| 缓冲大小 | `epd_buffer_size = (128/8)*296 = 4736`，但阅读/锁屏只提交 **4000 B** | `epd.h:4-5`、`ebook.c:690`、`ebook.c:962` |
| E2S 目标坐标 | `MP_Y(y) = y - 6`，逻辑帧 250×122 | `firmware/e2s_mp/main/mp_font.h:42-43` |

> 结论：**所有参考坐标先按 250×128 画，再整体上移 6 px** 才是 E2S 的 250×122。不要重新"设计"布局，否则行基线会差 1 行。

### 2.2 状态栏（READ 专用；菜单类屏幕只借几何）

| 元素 | 位置/规则 | 证据 |
|---|---|---|
| 左：书名/标题 | `x=EB_TEXT_MARGIN=2`，基线 `EB_BAR_TEXT_Y=22`，最大宽 `EB_TITLE_MAX_W = 180-2 = 178` | `ebook.h:59-74`、`ebook.h:85`、`ebook.c:760` |
| 右：电量（**仅 READ**） | 右对齐，`bx = 250-2-text_w`，下限 `EB_BATT_X0=180`；文本由 `ui_format_battery()` 生成（`100%`/`85%`/`7%`，充电时追加 `+`） | `ebook.c:762-771`、`battery.c:105-123` |
| 分隔线 | `obdRectangle(0, 27, 249, 28)` | `ebook.h:60-61`、`ebook.c:773` |
| SELECT 标题 | 左上是 `(N book)` 而不是书名，**不画电量** | `ebook.c:1056-1064` |
| SETTINGS/ABOUT 标题 | 字面量 `"Settings"` / `"About"`，**不画电量** | `ebook.c:1205-1207`、`ebook.c:1361-1363` |

所有非阅读屏共用同一套 y 坐标：基线 22 + 分隔线 27/28；菜单第一行基线从 44 起（§2.5）。

**状态栏没有页码、没有百分比、没有进度条**：`draw_status_bar()` 只画标题、电量、分隔线（`ebook.c:754-774`）。全文件对 `%` 的使用只在电量/日志格式串里，阅读路径没有任何"当前页/总页"计算（`grep -n "%\|page\|progress" ebook.c` 已核对，无页码输出）。

### 2.3 阅读页排版（核心不变量）

| 项 | GB2312 模式 | ASCII 模式 | 证据 |
|---|---|---|---|
| 每页行数 | **5** | **5** | `ebook.h:87` |
| 行高 | **18 px** | **19 px** | `ebook.h:92-93` |
| 首行定位 | 顶 `y=EB_CONTENT_TOP=30` | 基线 `EB_READ_ASCII_BASELINE0=43` | `ebook.h:66-68`、`ebook.c:807`、`ebook.c:795` |
| 第 n 行 | `y = 30 + 18n` → 30/48/66/84/102 | `baseline = 43 + 19n` → 43/62/81/100/119 | 同上 |
| 左右边距 | 2 px | 2 px | `ebook.h:85` |
| 可用行宽 | `EB_MAX_LINE_W = 246` | 246（按 `xAdvance` 累加） | `ebook.h:86` |
| 每行字数 | **最多 15 个全角**（16×15=240≤246，第 16 个越界） | 由比例字体决定 | `ebook.h:88`、`ebook_layout.h:40-58` |
| 单次读取上限 | 80 B | 80 B | `ebook.h:95` |
| 字库 | 外挂 flash `0x001000` 的 HZK16，32 B/字 | 编译进 flash 的 `Dialog_plain_16` | `ebook.h:6`、`ebook.c:643-645`、`ebook.c:794-795` |

**GB2312 模式下的 ASCII**：不走 8 px 半角，而是画 A3 区全角字模 `draw_hz_char(0xA3, c+0x80)` 并**按 16 px 前进**；注释解释了原因（A3 字形右笔画占 9..13 列，8 px 前进会压字）（`ebook.c:827-841`）。断行函数同口径：单字节也按 16 px 计（`ebook_layout.h:48-52`）。

**HZK16 取模**（与上传的字库格式绑定）：`区=hi-0xA0 (1..87)`、`位=lo-0xA0 (1..94)`、`offset=((区-1)*94+(位-1))*32`；16 行 × 2 字节，MSB 为最左（`ebook.c:636-659`）。

### 2.4 断行与分页（必须逐字节一致）

- `find_line_break_ascii`：`\n` 立即断；`\r` 跳过；不可打印按空格宽；超宽时退到**最后一个空格之后**，无空格则硬断（`ebook_layout.h:22-38`）。
- `find_line_break_gb2312`：`hi>=0xA1 && lo>=0xA1` 视为 2 字节全角；否则单字节也按 16 px；超宽即断（`ebook_layout.h:40-58`）。
- **没有首行缩进、没有避头尾、没有 CJK/西文混排**。缩进只能靠 TXT 自带全角空格。
- 渲染函数本身即分页器：返回"本页结束位置 = 下一页起点"；末尾提前收尾条件是 `brk >= to_read && to_read < 80`（`ebook.c:776-851`，`ebook.c:800`、`ebook.c:848`）。
- 下一页：**先**用 `render_page_*` 在 `char_pos` 调一次算出 `next_pos`（这一步只画进 RAM 虚拟屏，不提交面板），再入栈、改 `char_pos`、落盘、重画；若 `next_pos >= book_len` 说明已在最后一页，只重画当前页（`ebook.c:876-909`）。
- 上一页两条路径（`ebook.c:911-938`）：
  1. 本次会话翻过的页 → 16 深页首栈 `eb_page_hist[16]` 弹栈（`ebook.c:471-510`）；栈在普通 RAM，深睡后靠魔数 `0xEB11EB11` 检测并清空（`ebook.c:478-498`）。
  2. 栈空 → `eb_find_prev_page_start()` 纯布局反推（`ebook_layout.h:128-175`）：窗口 `[S-400, S+80)`（`EB_PAGE_BACK_MAX=5*80`），窗口到书头则从 0 正向走页链；否则从窗口起点**逐个候选**找"页尾恰好 == S"的最小候选；无匹配退化为"包含 S-1 的页"。
- 这条"无索引双向分页"是最值得原样复刻的算法；E2S 只需把 `eb_fill_win_t` 换成 SD `pread`。

### 2.5 SELECT / SETTINGS 菜单

| 项 | 规则 | 证据 |
|---|---|---|
| 行基线 | `44 + 19*row` → 44/63/82/101（`EB_MENU_BASELINE0 = 28+16`） | `ebook.h:63-64` |
| 可见行数 | 4 | `ebook.c:1068`、`ebook.c:1209` |
| 滚动 | `scroll = min(sel, total-4)`，sel 越界先 clamp | `ebook.c:1066-1072`、`ebook.c:1209-1212` |
| 选中标记 | 行首 `>`，未选中为空格；书名再左缩进到 x=12 | `ebook.c:1079-1095` |
| 环绕 | 顶部再上 → 末尾；底部再下 → 0 | `ebook.c:1157-1182`、`ebook.c:1276-1299` |
| SELECT 末项 | `Back to Clock`（14 字符 + 标记） | `ebook.c:1077-1083` |
| SETTINGS 六项 | `BT: On/Off` / `Sleep: Ns` / `GPart: On/Off` / `GC: N` / `About` / `Exit` | `ebook.c:1218-1247` |

### 2.6 时钟界面（默认 `epd_scene = 2`）

`epd_scene` 上电默认 **2**（`epd.c:45`），主循环走 `epd_display_time_with_date()`（`epd.c:421-432`、`epd.c:434-507`）。常量见 `ebook.h:75-84`。

**scene 2（默认，带日期/星期）**

| 元素 | 内容 | 位置 | 证据 |
|---|---|---|---|
| 顶栏左 | `MPP_%02X%02X%02X`（MAC 后 3 字节） | x=1，基线 22 | `epd.c:467-469` |
| 顶栏右 | `N%` / `N% +`（充电） | x=148，基线 22 | `epd.c:470-474` |
| 分隔线 | 全宽 | y=27..28 | `epd.c:476` |
| 大时间 | `HH:MM`，DSEG14 40 px 字体 | x=40，基线 70 | `epd.c:478-480`、`ebook.h:76-77` |
| 中分隔线 | 全宽 | y=74..76 | `epd.c:482` |
| 信息行 | `NmV  <BLE状态>`（无 mV 时 `--mV`） | x=4，基线 92 | `epd.c:484-488` |
| 下分隔线 | 全宽 | y=98..100 | `epd.c:490` |
| 日期 | `YYYY-MM-DD` | x=4，基线 116 | `epd.c:492-493` |
| 星期 | `Dialog_plain_16_zh`（ASCII 重映射的 16 px 中文字体）打印 `"9:%c"`：`'9'=星`、`':'=期`，码位 0x21..0x27/0x2D 为星期字；周日特判 `+6` | x=120，基线 120 | `epd.c:495-500`、`font16zh.h:128-173` |

**scene 1（可由 BLE `0xE1` 切换，`epd_display()`）**：顶栏 `MPP_XXXXXX <model>` + BLE 状态、"NmV BAT N%" 在 x=8/基线 110，无日期/星期（`epd.c:311-376`）。

> 星期行是"用 ASCII 码点索引中文点阵"的私有技巧，属于视觉实现；E2S 可以改用真正的字符串，只要语义（星期几）一致。时钟重绘节流见 §7.4。

### 2.7 锁屏

- 有有效图（`0x7FF000+0x0FF0 == 0xAA`）时：**整屏**读 4000 B 进 `epd_temp` → `FixBuffer` → 全刷；不叠加任何文字/白条（`ebook.c:953-963`、`ebook.c:974`）。
- 无图时：居中 `"Locked"`（x=60，基线 50）与 `"Hold F to wake"`（x=30，基线 72）（`ebook.c:964-972`）。
- 图像格式 = `epd_temp` 原格式：`idx=(y>>3)*250+x`，`bit(y&7)`，**1=黑**，共 4000 B（`web:1072-1083`）。
- 锁屏一律全刷并置 `epd_partial_ready = 1`（`ebook.c:974-975`）。

### 2.8 ABOUT

两页静态内容，page0 产品名 + `FW vX.Y.Z` + `Build:` + 日期 + 时间；page1 作者/邮箱/B站/Git + 4000 B 头像覆盖 + 页脚提示（`ebook.c:1355-1410`）。页脚基线 `EB_FOOTER_TEXT_Y = 122`（`ebook.h:69`）。

---

## 3. 按键契约

### 3.1 时序参数（LwBTN，`lwbtn_opts.h`）

| 参数 | 值 | 证据 |
|---|---|---|
| 按下去抖 | 20 ms | `lwbtn_opts.h:18` |
| 松手去抖 | 0 ms | `lwbtn_opts.h:19` |
| 最短点击 | 10 ms | `lwbtn_opts.h:20` |
| 最长点击 | **400 ms**（超过不算 click） | `lwbtn_opts.h:21` |
| 双击最大间隔 | **350 ms** | `lwbtn_opts.h:22` |
| keepalive 周期 | 100 ms | `lwbtn_opts.h:23` |
| 最大连续点击数 | 2 | `lwbtn_opts.h:26` |
| 到 2 击立即上报 | 1 | `lwbtn_opts.h:27` |
| 长按阈值 | `LONG_PRESS_MS 1000 / 100 = 10` 个 keepalive ≈ **1.0 s** | `ebook_buttons.c:27-28`、`ebook_buttons.c:118` |
| 采样节拍 | 10 ms（`ebook_button_tick` 自带限流） | `ebook_buttons.c:222-239` |

### 3.2 单击/双击判定窗口（必须按此实现，否则手感与主机预期不一致）

- 按下满 400 ms 后松手**不会**产生 click（`lwbtn.c:132-133` 的上界）。
- 第 1 次松手后 `click.cnt=1`，**不立即上报**；只有等于 2 时立即上报（`lwbtn.c:154-159`）。
- 若 350 ms 内没有第 2 击，空闲分支在计时到点时补发一次 ONCLICK（`lwbtn.c:164-171`）→ **所有键的单击都有 ~350 ms 延迟**。
- 双击在第 2 次松手时立即上报，`cnt` 归零（同上）→ 双击**没有** 350 ms 延迟。
- 长按在按住期间由 keepalive 计数触发，松手时 `long_fired[id]` 会丢弃随后的 ONCLICK（`ebook_buttons.c:112-119`、`ebook_buttons.c:143-150`）。注意：在当前配置下，长按超过 400 ms 本来就不会产生 click，"丢弃"逻辑只是双保险。

### 3.3 全模式按键表

| 模式 | F 单击 | F 双击 | L 单击 | R 单击 | F 长按 | L 长按 | R 长按 |
|---|---|---|---|---|---|---|---|
| CLOCK | — | — | 进 SETTINGS | 进 SELECT | LOCK | 进 SETTINGS | 进 SELECT |
| SELECT | 确认/打开 | — | 上移 | 下移 | LOCK | — | 回 CLOCK |
| READ | 下一页 | **上一页** | 上一页 | 下一页 | LOCK | — | 保存并退出 |
| SETTINGS | 切换/进入该项 | — | 上移 | 下移 | LOCK | 退出到 CLOCK | — |
| ABOUT | 回 SETTINGS | — | 上一页 | 下一页 | LOCK | — | — |
| LOCK | 解锁 | **不解锁**（无 handler） | — | — | 解锁 | — | — |
| DEBUG | — | — | — | — | LOCK | — | — |

证据：`ebook_buttons.c:58-93`（六个 click handler）、`ebook_buttons.c:117-140`（长按分派）、`ebook_buttons.c:155-163`（双击只对 F）。

### 3.4 硬件层

- 三键低有效，`gpio_read()?0:1`（`ebook_buttons.c:97-102`）。
- 引脚：F=PB4、L=PC4、R=PC0（`buttons.h:6-8`）。
- 长按后 `long_fired` 阻止重复触发；`ONPRESS` 时刷新 `last_activity_tick`（`ebook_buttons.c:112-115`）。
- 参考实现**不配置 GPIO 深睡唤醒**（浮空自唤醒问题，注释见 `ebook_buttons.c:213-219`），靠 BLE 广播周期醒来轮询。E2S 应直接用 `ext1`/`gpio` 唤醒，不要继承。

---

## 4. 目录与存储契约

### 4.1 8 槽 / 32 B entry

目录扇区 4 KB 位于外挂 flash `0x000000`，头 8 B + 8 × 32 B entry（`ebook.h:5`、`ebook.h:10`、`ebook.h:35-41`、`ebook.c:133-135`）：

| 偏移 | 长度 | 字段 | 说明 |
|---|---|---|---|
| 0x00 | 1 | `flags` | 见 4.2 |
| 0x01 | 20 | `title` | 写入方最多 19 B，第 20 B 保持 0（`ebook.c:105-110`、`ebook.c:430-435`） |
| 0x15 | 4 | `start` | LE32，书数据在 flash 的起始偏移 |
| 0x19 | 4 | `len` | LE32，字节长度 |
| 0x1D | 1 | `enc` | 0=ASCII，1=GB2312（`ebook.h:53-54`） |
| 0x1E | 2 | 保留 | 全 0 |

目录头：`'B','O','O','K'`、版本 `0x01`、`font_inst`、`book_cnt`（**从不更新，恒 0**）、`0`（`ebook.c:69-74`、`ebook.c:80-84`；`ebook.h:32-33`）。

空间布局（参考实现，E2S 换成文件即可）：`0x000000` 目录；`0x001000` 字库 261,696 B；`0x041000` 书区向上增长；`0x7FE000` 进度环；`0x7FF000` 锁图 + `0x0FF0` 有效标志（`ebook.h:5-16`、`ebook.h:26-29`）。

### 4.2 flags 状态机

```
EMPTY 0xFF --begin--> UPLOADING 0xFE --end--> DONE 0xFC
   ^                        |                     |
   +---- 删除/回收（整扇区重写）<-----------------+
```

- 只允许 1→0 的位翻转，所以状态标记本身不需要擦除（`ebook.h:43-47`）。
- "live" 定义：`DONE`，或 `UPLOADING` 且属于当前上传会话（`ebook.c:90-97`）。
- `DELETED = 0x00` **只是定义**，没有任何写入点；删除的实现是"擦扇区 + 只重写其它 DONE 项"，目标槽变成 `0xFF`（`ebook.c:244-278`）。
- 复用非空槽或回收僵尸 UPLOADING 时走 `catalog_rewrite_sector()` 整扇区重写（`ebook.c:125-159`、`ebook.c:416-423`）。
- 槽号 = 书号，选书列表按槽号升序，不按上传时间（`ebook.c:344-359`）。

### 4.3 标题与编码

- 固件不做解码；GB2312 模式按 `>=0xA1 && next>=0xA1` 判双字节，其余按单字节全角画（`ebook.c:820-841`）。
- 标题兜底：`enc != GB2312` 但标题里出现 `>=0xA1` 字节 → 自动按 GB2312 画（`ebook.c:699-704`）。
- 转码只发生在网页端：`txtDetectEncoding` 嗅探 BOM/UTF-8/GBK，`stringToGb2312` 转 GB2312，无法映射的字符写全角空格 `0xA1 0xA1`（`web:841-868`）。
- 书名编码**独立于正文编码**：`catalogTitleEncoding()` 在 ASCII 模式下遇到非 ASCII 字符会强制升为 GB2312（`gb2312_codec.js:5-9`、`web:913-925`）。
- 标题截断到 19 字节（`web:919`、`ebook.c:107`、`ebook.c:432`）。

### 4.4 上传 / 完成 / 删除（设备侧语义）

| 阶段 | 设备动作 | 证据 |
|---|---|---|
| BEGIN(0x10) | 取上传锁 → 等 EPD 空闲 ≤600 ms（拿不到回 `0xFE`）→ 找空槽（3 轮，含回收）→ 空间检查 → 写 `UPLOADING` entry（含标题/start/len/enc）→ 请求 7.5 ms 连接间隔 → 回 `[0x10][0][slot]` | `ebook_ble.c:161-178`、`ebook_ble.c:180-240` |
| DATA(0x11) | 累计到 256 B 才落盘；按 4 KB 边界预擦；每累计 256 B 回一次 `[0x11][written24]` | `ebook_ble.c:51-126`、`ebook_ble.c:242-253` |
| END(0x12) | flush 暂存 → 回一次 `[0x11][written]` → 写 `DONE` 标志（会等 EPD ≤200 ms）→ 回 `[0x12][0][slot][written24]` | `ebook_ble.c:255-283` |
| 空间不足 | `start + total > 0x800000` → `0x02`（**未扣除 0x7FE000/0x7FF000 两个保留扇区**） | `ebook_ble.c:215` |
| 8 槽满 | `0x01` | `ebook_ble.c:205-211` |
| DELETE(0x13) | 整扇区重写（丢掉该槽）→ 追加一条 `pos=0` 的进度记录 → 回 `[0x13][0]` | `ebook_ble.c:285-299`、`ebook.c:244-278`、`ebook_ble.c:296` |

**不做长度校验**：`0x12` 不比对 `written` 与 `total`（`ebook_ble.c:255-283`），`0x18` 不比对字库总长（`ebook_ble.c:670-686`）。截断的上传会被标成 DONE。E2S 必须补校验（§10）。

### 4.5 UI 分页/滚动（与 4.1 的联动）

- 列表只显示 `DONE`，`count` 现扫（`ebook.c:344-364`）。
- 光标 0..count-1 是书，`count` 是 "Back to Clock"；`eb_selected > cnt` 时 clamp（`ebook.c:1053`）。
- 4 行可见 + 滚动 + 环绕（§2.5）。
- 删除/新增后不自动刷新列表；README 要求用户"上传后点刷新列表"（`readme.md:83`）。

---

## 5. 进度环（每本书的阅读位置）

### 5.1 记录格式（参考实现用 NOR，E2S 用文件/NVS，但字节格式可照抄）

固定 8 B，只追加（`ebook.h:18-29`、`ebook.c:1464-1583`）：

| 偏移 | 值 |
|---|---|
| 0 | magic `0x5A` |
| 1 | book_idx（0..7） |
| 2..5 | `char_pos` LE32 |
| 6 | `0x00` |
| 7 | XOR of bytes 0..6（= 前 6 字节的 XOR，因为第 6 字节恒 0） |

- 写：找第一个**全 0xFF** 的 8 B 槽；满了就 compact（每本书保留最新一条 → 擦扇区 → 重写）再写（`ebook.c:1482-1529`）。
- 读：从头线性扫，遇到第一个 `magic != 0x5A` 就停；取该书**最后一条** CRC 通过的记录（`ebook.c:1558-1583`）。
- 容量：512 条/4 KB（`ebook.h:27-28`）。

### 5.2 保存时机（全部）

| 时机 | 调用 | 证据 |
|---|---|---|
| 下一页（翻页成功后） | `ebook_progress_save(book_idx, new_pos)` | `ebook.c:905` |
| 上一页 | 同上 | `ebook.c:935` |
| READ 长按 R 退出 | 同上 | `ebook.c:606` |
| BLE 删除书 | `ebook_progress_save(idx, 0)` | `ebook_ble.c:296` |
| 打开书 | **只读不写** | `ebook.c:571-581` |

- `ebook_progress_save()` 在 `ext_flash_is_safe()` 为假时**静默 return**（`ebook.c:1534`）→ 翻页恰好撞上刷新会丢一次保存，靠下一次翻页补。
- 恢复：`ebook_open()` 里 `ebook_progress_load()`，失败则 0（`ebook.c:571-577`）；上电不自动续读（`ebook.c:528-548`）。
- settings 里的 `ebook_char_pos/prev` 只是镜像，用于退出路径；真正的真源是进度环（`ebook.c:897-905`、`ebook.c:601-606`）。

### 5.3 原版是否显示页码/百分比/进度条

**都没有。**
- 设备端：状态栏只有标题 + 电量（`ebook.c:754-774`）；阅读页只有 5 行正文；没有"第 N 页"、没有百分比、没有进度条。全书也没有页码概念（分页是纯字节偏移）。
- 网页端：只有上传进度条（`bookBar`/`fontBar`，`web:912`、`web:810`）和"Flash 已用/7168 KB"的粗略估算（`web:1007-1013`），不是阅读进度。

---

## 6. 设置项

### 6.1 定义与默认值

`settings_struct` 存内部 flash `0x78100`，整结构 + `magic 0xABCFF125` + `len` + 末字节 XOR CRC（`flash.c:11`、`flash.c:19-28`、`flash.c:55-70`、`flash.h:3-26`）。

| 设置 | 字段 | 默认 | 选项 | 谁能改 | 生效 | 证据 |
|---|---|---|---|---|---|---|
| BLE 广播 | `ble_enabled` | **1（开）** | 0/1 | 菜单第 1 项；BLE `0x35` | 立即 `ble_set_advertising()` + 落盘 | `flash.c:49`、`ebook.c:1305-1310`、`ebook_ble.c:605-622` |
| 空闲休眠 | `sleep_timeout_idx` | **2 = 60 s** | {15,30,60,120} s | 菜单第 2 项循环；BLE `0x35`（越界 clamp 到 2） | 下一轮主循环读 | `flash.c:14`、`flash.c:50`、`app.c:158-162`、`ebook_ble.c:610-612` |
| 全局局刷 | `epd_partial_enabled` | **0（关：场景切换全刷）** | 0/1 | **仅菜单第 3 项** | 刷新决策函数 | `flash.c:51`、`ebook.c:1317-1321`、`epd_refresh.c:24-58` |
| GC 间隔 | `epd_gc_interval_idx` | **0 = 10** | {10,20,50,100} | **仅菜单第 4 项** | 刷新决策函数 | `flash.c:15`、`flash.c:52`、`ebook.c:1322-1327`、`epd_refresh.c:10-18` |

注意：
- `0x35` 只带 `[ble_enabled][sleep_idx]`，**无法**改局刷/GC（`ebook_ble.c:605-608`）；`0x34` 也只回 BLE/sleep/版本（`ebook_ble.c:590-601`）。
- 上电时 `init_ble()` 一律先开广播，然后再按 `ble_enabled` 关掉——保证"关掉 BLE 后还能被网页重新打开"（`app.c:42-49`）。
- 菜单改动**不重绘时钟**：`ebook_render_settings()` 重画菜单本身（`ebook.c:1302-1337`）；退出设置才全刷时钟（`ebook.c:1266-1272`）。

### 6.2 设置菜单行为

- 光标进菜单时归零（`ebook.c:1260`）。
- 第 1/2/3/4 项：F 单击循环/切换并立即落盘（`ebook.c:1305-1327`）。
- 第 5 项 About：F 进入；第 6 项 Exit：F 退出（`ebook.c:1328-1333`）。

---

## 7. 刷新策略

### 7.1 决策函数 `epd_refresh_pick(scene, allow_partial)`

`epd_refresh.c:32-69`，伪代码：

```
if (!epd_partial_ready) return FULL;                 // 没有基准图 → 必须全刷
if (force_next[scene]) { force_next=0;
    if (!partial_enabled) { since_full[scene]=0; return FULL; }
    // partial_enabled=1 时：场景切换当普通局刷，继续往下
}
if (partial_enabled) {                               // 全局局刷模式
    if (++since_full_global >= gc_interval) { since_full_global=0; return FULL; }
    return PARTIAL;
}
if (!allow_partial) return FULL;                     // 当前所有调用点都传 1
if (++since_full[scene] >= gc_interval) { since_full[scene]=0; return FULL; }
return PARTIAL;
```

- 场景：`CLOCK=0, READ=1, MENU=2`（`epd_refresh.h:9-14`）。READ 在 `ebook_open()` 进入；MENU 在 SELECT/SETTINGS 进入；CLOCK 由 `set_EPD_wait_flush()` 进入（`ebook.c:565`、`ebook.c:1114`、`ebook.c:1261`、`epd.c:103-113`）。
- `epd_refresh_scene_enter()`：置 `force_next`；全局局刷开着时**保留**基准图与计数器；否则清 `since_full` 并 `epd_partial_ready=0`（`epd_refresh.c:20-30`）。
- `set_EPD_wait_flush()` 强制 `epd_partial_ready = 0`（`epd.c:103-113`）；`ebook_handle_lock()` 也清（`ebook.c:993`）；解锁回 READ 也清（`ebook.c:1027`）。理由写在注释里：面板每次刷新后深睡，**只发 0x00 唤醒的局刷会静默失败**（`epd.c:106-112`）。
- 全刷后 `ebook_commit_display()` 才置 `epd_partial_ready = 1`（`ebook.c:684-693`）；时钟路径在 `update_time_scene()` 里置（`epd.c:403-418`）。

### 7.2 面板动作

| 模式 | 序列 | 证据 |
|---|---|---|
| 全刷 `fp=1` | HW reset(10/10 ms) → `0x12` SW reset → 几何(0x01/0x11/0x44/0x45/0x3C/0x21) → `0x18=0x80` → 写 `0x24` 与 `0x26` 同一帧 → `0x22=0xF7` → `0x20` → 等 BUSY ≤3000 ms → `0x10=0x01` 深睡 | `epd_bw_213.c:71-98`、`epd_bw_213.c:152-163` |
| 局刷 `fp=0` | **不 reset、不深睡** → `0x00` 唤醒 → 重设几何 → 只写 `0x24` → `0x22=0xFF` → `0x20` → 等 BUSY | `epd_bw_213.c:163-173` |
| 每次刷新后 | `epd_update_state=1` + 记时；`epd_state_handler()` 等 BUSY 释放 → `epd_set_sleep()` → `epd_update_state=0` | `epd.c:207-208`、`epd.c:211-254` |

`FixBuffer()` 把 OBD 行式缓冲（250 列 × 16 行字节）旋转 90°、取反、位反转成面板列式（`epd.c:256-270`）。

### 7.3 忙碌 / 超时 / 温度 / 失败

- **忙碌门**：`epd_update_state` 是全局唯一忙标志；`ext_flash_is_safe()` 就是它的反（`ext_flash.c:8-11`）。忙时所有渲染/存储入口要么等待（`catalog_wait_flash` 600 ms、写 entry 200 ms、锁图 50 ms、begin 600 ms），要么置 `render_pending`。
- **8 s BUSY 看门狗**：BUSY 一直不释放就强制 `epd_set_sleep()` 并把状态清零，避免"永久冻结"（`EPD_STATE_BUSY_TIMEOUT_US = 8000000`，`epd.c:42`、`epd.c:236-244`）。
- **面板级等待超时是静默的**：`EPD_CheckStatus()` 超时直接 return，没有错误码、不重试（`epd_spi.c:102-112`）。
- **温度**：`EPD_BW_213_read_temp()` 读 `0x18/0x22=0xB1/0x20` 后**直接 return 25**（`epd_bw_213.c:115-124`）；时钟底部信息行注释明确"panel has no real sensor"（`epd.c:368-373`）。广播里的温度来自同一个假值（`app.c:88-90`）。参考实现**没有**按温度调 LUT/波形。
- **上传期间**：主循环停掉时钟刷新与自动锁屏（`app.c:99-104`、`app.c:117`），因为刷新会抢共享 SPI 并把 `ext_flash_is_safe()` 打成假，导致数据块被静默丢弃（注释 `app.c:99-102`）。
- **失效语义**：`ebook_progress_save()` 忙则丢一次；`ebook_catalog_set_flag()` 忙则静默不写（`ebook.c:451-461`）——`0x12` 里已用 200 ms 等待兜底（`ebook.c:264-271`）。

### 7.4 时钟 / 锁屏刷新节奏

- CLOCK：每轮主循环调 `epd_update()`，但只有"分钟变了"或 `epd_wait_update` 才真正画（`epd.c:393-419`、`app.c:103-104`）。
- 锁屏：进入时全刷一次；锁屏期间不刷新（`app.c:105-106` 只扫描按键）。
- 解锁：READ → 全刷当前页；其它 → 全刷时钟（`ebook.c:1023-1036`）。

---

## 8. BLE 契约

### 8.1 GATT 与服务

| 用途 | Service | Characteristic | 属性 | 证据 |
|---|---|---|---|---|
| 阅读器/字库/锁图/设置/OTA | `0x221F` | `0x331F`（`OTA_CMD_OUT_DP_H`） | Write + Notify | `app_att.c:75-78`、`app_att.c:139-143`、`app_att.c:189-192` |
| 透传（对时/日志/调试） | `0x1F10` | `0x1F1F`（`RxTx_CMD_OUT_DP_H`） | Write + Notify | `app_att.c:81-84`、`app_att.c:146-150`、`app_att.c:195-198` |
| 电池 | `0x180F` | `0x2A19` | Read + Notify | `app_att.c:63-66`、`app_att.c:125-129` |
| 温度 | `0x181A` | `0x2A1F` | Read + Notify | `app_att.c:69-72` |
| 旧 EPD 帧缓冲 | `13187B10-…` | `4B646063-…` | Write + Notify | `app_att.c:88-94`、`app_att.c:200-203` |

- **设备名特征 = `"ESL"`**（`app_att.c:58`）；**广播/扫描响应名 = `MPP_` + MAC 后 3 字节**（`ble.c:56`、`ble.c:237-243`、`ble.c:265`）。网页按 `MPP` 前缀过滤，认的是后者。
- 广播数据是 0x181A 的 Service Data（len 16）：MAC6 + temp2 + humi1 + batL1 + batmV2 + counter1（`ble.c:58-65`、`ble.c:295-308`）。
- 安全：`No_Security`（`ble.c:263`），不配对不加密。

### 8.2 路由（决定"哪些 opcode 能到应用层"）

```
OTA 特征写入 → otaWritePre()
    首字节 0x10..0x44  → epd_ble_handle_write() → default → ebook_ble_handle_command()
    其它                → custom_otaWrite()（固件 OTA：0x00..0x07）
```

`ble.c:192-203`、`epd_ble_service.c:40-85`。

**这条规则是扩展协议时最硬的约束**：新 opcode 若不在 `0x10..0x44`，在参考固件上会掉进 OTA 分支。范围内未占用的号段：`0x1B-0x1F`、`0x22-0x2F`、`0x36-0x3F`。

### 8.3 广播 / MTU / 连接参数

| 项 | 值 | 证据 |
|---|---|---|
| 广播间隔 | 1600 × 0.625 ms = **1 s** | `app_config.h:9`、`ble.c:266` |
| ATT MTU 请求 | **247**，连接后 1.5 s 与 3.0 s 各一次 | `ble.c:34-35`、`ble.c:133-143` |
| 本地 RX MTU | `blc_att_setRxMtuSize(247)` | `ble.c:279` |
| DLE | 2.0 s 请求 251 | `ble.c:36`、`ble.c:145-149` |
| 连接间隔 | `ble_set_connection_speed(6)` → `{6,8,0,2000}` = 7.5–10 ms | `ble.c:187-190`、`ble.c:183`、`ebook_ble.c:234` |
| 单包 ATT 载荷 | MTU−3（247→244），数据包再减 1 字节 opcode | `web:332-338`、`web:759-767` |

### 8.4 命令表（op / 方向 / 载荷 / 应答）

> 方向 `H→D` = 主机写，`D→H` = 设备 notify。所有多字节整数除特别注明外均为**大端**（代码逐字节手工拼装，例如 `ebook_ble.c:277-279`）。

| op | 方向 | 请求载荷 | 设备应答 | 证据 |
|---|---|---|---|---|
| `0x10` 书-开始 | H→D | `[title_len u8][title…][total u24 BE][enc u8]` | `[0x10][status][slot]` | `ebook_ble.c:180-240` |
| `0x11` 书-数据 | H→D | `[raw…]`（1..244 B） | 每累计 256 B：`[0x11][written u24 BE]` | `ebook_ble.c:242-253`、`ebook_ble.c:89-112` |
| `0x12` 书-结束 | H→D | `[]` | `[0x12][status][slot][written u24 BE]` | `ebook_ble.c:255-283` |
| `0x13` 删书 | H→D | `[idx]` | `[0x13][status]` | `ebook_ble.c:285-299` |
| `0x14` 书数 | H→D | `[]` | `[0x14][count]` | `ebook_ble.c:345-372` |
| `0x15` 查单本 | H→D | `[idx]` | 3 条通知：`[0x15][idx][0x00][title 0..11]`(15 B) / `[0x15][idx][0x01][title 12..19]`(11 B) / `[0x15][idx][0x02][len u24][enc]`(7 B)；空槽 `[0x15][idx][0xFF]`；忙 `[0x15][idx][0xFE]` | `ebook_ble.c:374-439` |
| `0x16` 字库-开始 | H→D | `[total u24 BE]` | `[0x16][status]` | `ebook_ble.c:626-655` |
| `0x17` 字库-数据 | H→D | `[raw…]` | 每 256 B：`[0x17][written u24 BE]` | `ebook_ble.c:657-668` |
| `0x18` 字库-结束 | H→D | `[]` | `[0x18][status]`；成功时置目录头 `font_inst=1` | `ebook_ble.c:670-686` |
| `0x19` 状态 | H→D | `[]` | `[0x19][font_ok][book_cnt][lock_ok][att_mtu u8]` | `ebook_ble.c:301-333` |
| `0x1A` 目录压缩 | H→D | `[]` | `[0x1A][0x00]` | `ebook_ble.c:335-343` |
| `0x20` 打开书 | H→D | `[idx]` | `[0x20][status]`（0=已 active，0xFF=失败） | `ebook_ble.c:690-700` |
| `0x21` 关闭书 | H→D | `[]` | `[0x21]`（无状态字节） | `ebook_ble.c:702-707` |
| `0x30` 锁图-开始 | H→D | `[total u24 BE]`，**必须 == 4000** 否则 `0xFD`（短包回 `0xFF`） | `[0x30][status]` | `ebook_ble.c:443-467` |
| `0x31` 锁图-数据 | H→D | `[off u16 BE][data…]`，`off+len ≤ 4000` | `[0x31][status]` | `ebook_ble.c:469-488` |
| `0x32` 锁图-结束 | H→D | `[]` | `[0x32][status]`；成功写 `0xAA` 到 `0x7FF000+0x0FF0` | `ebook_ble.c:490-503` |
| `0x33` 回读锁图 | H→D | `[off u16 BE]`（推荐）或空（legacy 全量流） | 推荐：`[0x33][off u16 BE][data ≤160]`；`off≥4000` → `[0x33][0xFF][0xFF]`；legacy：`[0x33][0xFB]` 心跳 → `[0x33][0xFC][flag]` → 23×`[0x33][off][176B]` → `[0x33][0xFF][0xFF]` | `ebook_ble.c:505-587` |
| `0x34` 读设置 | H→D | `[]` | `[0x34][ble][sleep_idx][fw_major][fw_minor][fw_patch]` | `ebook_ble.c:589-601` |
| `0x35` 写设置 | H→D | `[ble][sleep_idx]` | `[0x35][status]` | `ebook_ble.c:603-622` |
| `0x40` 调试画线 | H→D | `[y u16 BE]`（clamp ≤127） | `[0x40][status][y_clamped]`，全刷 | `ebook_ble.c:719-745` |
| `0x41` 退出调试 | H→D | `[]` | `[0x41][0x00]`，回 CLOCK | `ebook_ble.c:747-755` |
| `0x42` PC1 弱下拉 | H→D | `[]` | `[0x42][level]` | `ebook_ble.c:801-805` |
| `0x43` PC1 强下拉 | H→D | `[]` | `[0x43][level]` | `ebook_ble.c:807-811` |
| `0x44` PC1 高阻 | H→D | `[]` | `[0x44][level]` | `ebook_ble.c:813-817` |
| `0x00`/`0x01`/`0x02`/`0x03`/`0x04` | H→D（**仅 EPD_BLE 特征**） | 旧 EPD 推屏：`0x00` 整屏填色、`0x01` 推缓冲刷新、`0x02` 设 byte_pos、`0x03` 写缓冲（2 B 偏移）、`0x04` TIFF 解码 | `0x03` 回 2 B 长度，其余无 | `epd_ble_service.c:40-85` |

`0x11`/`0x17` 的应答是**累计已写字节数**（高水位），不是"本包 ack"；丢一条通知只会让窗口晚一点放开（`ebook_ble.c:89-97`、`web:484-503`）。

### 8.5 状态码

| 码 | 含义 | 出现在 |
|---|---|---|
| `0x00` | OK | 全部 |
| `0x01` | 8 槽已满 | `0x10`（`ebook_ble.c:205-211`） |
| `0x02` | flash 空间不足 | `0x10`（`ebook_ble.c:215`） |
| `0xFB` | legacy 0x33 心跳 | `0x33`（`ebook_ble.c:561-562`） |
| `0xFC` | legacy 0x33 标志位 | `0x33`（`ebook_ble.c:566-567`） |
| `0xFD` | 包长/参数非法 | `0x10`（`ebook_ble.c:183/192`）、`0x30`（`ebook_ble.c:451`） |
| `0xFE` | EPD 忙 / 请重试；`0x15` 里也表示"忙" | `0x10`、`0x15`、`0x30`、`0x33` |
| `0xFF` | 失败 / 槽不存在 / 未开始上传 / 锁图块写失败 | `0x10` 初值、`0x13`、`0x15`（absent）、`0x18`、`0x20`、`0x31`、`0x32`、`0x35` |

### 8.6 上传流控（设备侧）

- 暂存 256 B，满了才 `erase_ahead` + 分页写（`UPLOAD_STAGING_SIZE/UPLOAD_NOTIFY_STEP`，`ebook_ble.c:76-112`）。
- 擦除按 `next_erase` 单调推进，4 KB 一次，跨请求不重复擦（`ebook_ble.c:51-58`）。
- 写完每 256 B 主动 notify 一次累计值（`ebook_ble.c:110-111`）。
- 上传会话期间 `app.c` 停时钟刷新/自动锁屏（`ebook_ble.c:25-28`、`app.c:99-104`、`app.c:117`）。
- 断连回调会 `ebook_ble_reset_upload()` 清会话并回收僵尸槽（`ble.c:163-170`、`ebook_ble.c:33-37`）。

### 8.7 时间同步（RxTx 0x1F10/0x1F1F）

```
H→D  [0xDD][unix32 BE][year u16 BE][month][day][week]
```

- 解析：`new_time = dat1<<24 | dat2<<16 | dat3<<8 | dat4`；`set_time(new_time, dat5<<8|dat6, dat7, dat8, dat9)`（`cmd_parser.c:73-75`、`etime.c:78-85`）。
- `year` 是"十进制年份的十六进制写法"（2026 → `0x07EA`），网页用 `y.toString(16)` 生成（`web:719-726`）。
- `week`：`getDay()||7`，即 **1=周一 … 6=周六，7=周日**（`web:722`）；固件原样存 `tm_week`。
- `unix` 是**本地时间减时区偏移**后的秒数（`web:721`），即设备把本地时间当 UTC 跑；固件不做时区换算。
- 固件时间基准是上电默认 `1783699200`（`etime.c:23` 注释写 2026-07-11 00:00:00）+ 每秒自增（`etime.c:20-31`、`etime.c:33-61`），掉电不保存，必须每次连上后同步。
- 同一特征还承载：调试文本日志（`ble_log()` 同时推 0x1F1F 与 0x331F，`ble.c:88-99`）、`RxTxWrite()` 先把首字节回显成 `[R X <op>]` 再交给 `cmd_parser`（`ble.c:205-213`）。
- `cmd_parser` 里还有 `0xE2` 强制刷新、`0xED` 按键扫描、`0xF3/F4/F5` flash 诊断等调试命令（`cmd_parser.c:82-239`），不属于阅读协议，E2S 可选。

### 8.8 已知怪癖 / 陷阱（实现时必须决定"照抄还是修正"）

1. `0x15` 的 `len` 只发 **24 bit**（`p3[3..5]`），丢掉了 32 位里的最高字节（`ebook_ble.c:412-432`）；书 ≤8 MB 时无影响。
2. `0x14` 在 EPD 忙时回 `count=0`，不报错（`ebook_ble.c:350-355`）→ 主机看到"0 本书"。
3. `0x19` 在 EPD 忙时回全 0（`ebook_ble.c:306`）→ 主机误判"字库未装/无锁图"。
4. `0x11` 在 `upload.active != 1` 时回 `written=0`（`ebook_ble.c:245`）；主机若只看高水位会以为"卡住"。
5. `0x12` 不校验 `written == total`（`ebook_ble.c:255-283`）→ 截断上传被标 DONE。
6. `0x18` 不校验字库长度，也不校验 `261696`（`ebook_ble.c:670-686`）。
7. `0x30` 只接受 4000，其它长度 `0xFD`；但 `0x31` 允许乱序/覆盖写（`ebook_ble.c:469-488`），没有序号/CRC。
8. `0x33` 的**请求-应答模式不检查有效标志**：无锁图时也会返回全 0 数据块，只有 legacy 模式才会回 `[0x33][0xFE]`（`ebook_ble.c:514-587`）。
9. `0x10` 的空间检查用 `EB_FLASH_END`，未扣保留扇区（`ebook_ble.c:215`），**单本 >7.7 MB 会踩进度环/锁图**；网页用 7168 KB 规避（`web:1009`）。
10. `0x20` 的 `0xFF` 既可能"目录读失败"也可能"EPD 忙"（`ebook_ble.c:690-700` 只看 `ebook_state.active`）。
11. 固件在所有刷新/写入路径都用 `WaitUs/WaitMs` 阻塞 BLE 回调，最长 600 ms（begin）——主机超时不能设太短。

---

## 9. 原版 web tool 的发现 / 连接 / 上传

### 9.1 发现与连接

| 步骤 | 行为 | 证据 |
|---|---|---|
| 过滤 | 默认 `filters=[{namePrefix:'MPP'}]`；勾选"不筛选"则 `acceptAllDevices` | `web:280-285` |
| 必需服务声明 | `optionalServices = [0x221F, 0x1F10]` | `web:283` |
| 连接 | `gatt.connect()` → 若支持 `requestMTU` 则 247（Android），否则等设备主动协商（Windows） | `web:289-297` |
| 取特征 | `0000221f…` → `0000331f…`；再取 `00001f10…` → `00001f1f…` | `web:299-300`、`web:322-327` |
| 订阅 | 两个特征都 `startNotifications()` | `web:306-309`、`web:325` |
| 连接后 2 s | 自动 `setTime()` + `queryStatus()` + `bookList()` + 轮询 payload | `web:312` |
| 收到 `0x19` 后 | 顺带触发 `loadLockImg()`（0x33 链）与 `loadSettings()`（0x34） | `web:525-526` |
| payload 上限 | `maxWriteValueLength()`，未协商时 20，协商后 244 | `web:334-356` |

### 9.2 上传流程（书）

1. 选文件 → `txtPrepareBookFile()`：`.txt` 嗅探 BOM/UTF-8/GBK，必要时转 GB2312；`.bin` 原样当 GB2312（`web:873-898`）。
2. 大于 7 MB 直接拒绝（`web:911`）。
3. 书名编码 + 截断 19 B（`web:913-925`）。
4. `0x1A` 压缩目录：等 ack（3 s）但**不看状态字节**（`web:826-828`）。
5. `0x10` 开始：`[0x10][title_len][title][len24][enc]`，等 ack（5 s），按状态码报错（`web:927-940`）。
6. 数据：`streamUpload(0x11, …)`（见 9.3）。
7. `0x12` 结束，等 10 s；**不检查 ack 状态**，直接显示"✅ 完成"（`web:944-945`）。
8. 500 ms 后刷新列表 + 1 s 后查状态（`web:948`）。

### 9.3 分片 / 窗口 / 重试

| 项 | 值 | 证据 |
|---|---|---|
| 每包数据 | `blePayloadMax - 1`（opcode 占 1 B），最小 20 | `web:767` |
| 写方式 | `writeValueWithoutResponse`（Write Command），靠 `await` 在链路层限速 | `web:362-367`、`web:787` |
| 窗口 | `min(8192, pktWindow*chunkSz)`；`pktWindow = payload≤20 ? 48 : 24` | `web:753`、`web:769-770` |
| 窗口满 | `waitMs(4)` 后重查，不中止 | `web:782-785` |
| 排空 | 最多等 1500 ms，**不要求** `fwWritten==total` | `web:797-798` |
| 单槽查询重试 | 3 次，间隔 150 ms | `web:952-966` |
| 列表补查 | 缺失槽再扫一轮（8 次） | `web:995-1002` |
| 书/font 重传 | **没有** | — |
| `0xFE`（EPD 忙） | 只提示"稍后重试"，不自动重试 | `web:936` |

### 9.4 CRC 在哪里

- **书 / 字库 / 锁图上传全程没有 CRC**。完整性只靠 `0x12`/`0x18` 的 `written` 计数（而固件又不比对 total）。
- 只有**固件 OTA** 有"CRC"：`calculateCRC()` 把 0x40000 范围内偏移为偶数的字节累加成 16 位（`web:1259-1267`），随 `07 C001CEED <crc>` 发送（`web:1269-1271`）；固件侧对 0x20000 起 128 KB 做同样的字节累加（`ota.c:79-92`、`ota.c:93-96`）。
- 网页注释明确说"不要在 CRC 校验后发 0x06"，否则 `crc_out` 非 0 会让最终写入的守卫条件失败（`web:1246-1253`）。

### 9.5 主机侧会"假成功"的地方（E2S 主机不要复刻）

| 位置 | 现象 | 证据 |
|---|---|---|
| 字库上传 | `0x16` 只 `await` 写入、**不等 ack**；若固件回 `0xFE`，数据被忽略，最后 `0x18` 回 `0xFF` 但页面仍显示"✅ 完成" | `web:815`、`web:819` |
| 书上传 | `0x12` 的 ack 不检查 `[1]`，`0xFF` 也显示"✅ 完成" | `web:945` |
| 锁图上传 | `0x31` 每块用 Write Request 等回包，但 `0x32` 不检查状态 | `web:1118-1126` |
| 锁图分片 | `imgChunk = max(40, payload-3)`：MTU 未协商时每包 43 B，超过 payload=20 的写上限，`writeValue` 会直接抛错 | `web:1118` |
| 进度条 | 以"已发出字节"为准，不是"已落盘" | `web:789` |

---

## 10. 哪些 E2S 能力可以增强而不破坏兼容

### 10.1 硬不变量（改了就不是"兼容 Miaopaper"）

1. `0x221F/0x331F` 与 `0x1F10/0x1F1F` 的 UUID、`MPP_` 前缀广播名、No-Security。
2. opcode 语义与字节布局（§8.4）、状态码（§8.5）、**大端**整数、`0x11/0x17` 的"累计 written"语义。
3. `0x10` 的 `title_len ≤ 19`、`enc` 取值 0/1、目录 32 B entry 字段偏移。
4. 阅读排版常量（5 行、18/19 px、246 px、每行 80 B 上限）与断行/回退算法——**主机侧分页与"上一页"依赖它**。
5. 按键语义（F/L/R 的三类手势）与 1.0 s 长按、350 ms 双击窗口；至少要让原版用户的肌肉记忆成立。
6. 场景切换全刷 + `epd_partial_ready` 规则（否则局刷会静默失败，这是面板特性不是偏好）。

### 10.2 可以增强（对主机透明或只增不改）

| 能力 | 做法 | 兼容性论证 |
|---|---|---|
| **5 键** | `OK→F`、`PREV→L`、`NEXT→R`；`MENU`/`FUNC` 作为**新增入口**（如 MENU 从任意界面进设置、FUNC 直接锁屏） | 原三键语义不变；新增键不改变既有手势。`firmware/e2s_mp/CMakeLists.txt:13` 已把长按设为 1000 ms；`e2s_reader` 的 `key_id_t` 就是这 5 个键（`components/input/include/input.h:12-17`） |
| **内置/SD 存储** | 逻辑对象（目录/书/字库/锁图/进度/设置）不变，物理载体换文件 | 参考实现的协议里没有任何 flash 地址；`mp_store.h:1-17` 已按此映射。注意目录 entry 的 `start` 字段在 E2S 无意义（可填 0 或槽号），主机不解析它 |
| **阅读进度条/百分比** | 在状态栏或页脚新增元素；数值由"当前 `char_pos` / `book_len`"算 | 主机不读屏幕；只要不改 `char_pos` 语义与页边界即可。注意参考状态栏右侧 180..248 已被电量占用，页脚基线 122 只剩 ~6 px，实际要挤进正文第 5 行下方需重排——建议放到状态栏左侧标题之后，或用图标 |
| **空闲局刷** | 时钟/锁屏在空闲时按 N 次局刷做一次全刷 GC；可加 RTC 定时唤醒 | 与参考的 `gc_interval` 同构；只要保留"场景切换全刷 + 无基准图不局刷" |
| **真实温度** | 用 E2S 传感器替代硬编码 25，写进广播 Service Data 的 temp 字段 | 字段已存在（`ble.c:58-65`），主机只是显示 |
| **严格上传校验** | `0x12` 前比对 `written==total`，不一致回新状态码；可选 CRC32 | 需要**新增**状态码时，主机旧解析会落到"未知错误"分支（`web:939`），不会崩；更稳妥是新开 `0x2x` 能力查询 |
| **断点续传** | 新 op（如 `0x22`）返回已写偏移 | 旧主机不会发，天然兼容 |
| **上传中止** | 新 op（如 `0x1B` ABORT） | 旧主机不感知；`ebook_ble_reset_upload()` 已有清理逻辑可复用（`ebook_ble.c:33-37`） |
| **多语言/UTF-8** | 设备端新增 `enc=2` | **会破坏**：参考固件只认 0/1，旧网页也不会发 2；若加，必须让 0/1 行为完全不变，且 `0x15` 的 enc 字节对旧主机仍可解释 |

### 10.3 明确不要照抄

- 外挂 flash 的"单向增长 + 删除不回收"（`ebook.c:366-393`）。
- `catalog_rewrite_sector` 每次 begin/delete 都擦一次目录扇区（`ebook.c:125-159`、`ebook.c:260`）。
- 无 GPIO 唤醒的休眠策略（`ebook_buttons.c:213-219`）。
- 硬编码温度 25（`epd_bw_213.c:123`）。
- `tiffg4.c`（GPLv3，`tiffg4.c:9-19` 的许可头）——阅读器不需要 TIFF 路径。
- 假成功的主机逻辑（§9.5）。

### 10.4 建议的 E2S 侧新增 op（不冲突，且在 0x10..0x44 内）

| 建议 op | 用途 | 兼容性 |
|---|---|---|
| `0x1B` | UPLOAD_ABORT | 旧主机不发 |
| `0x22` | QUERY_RESUME（返回 `written`） | 旧主机不发 |
| `0x36` | CAPS（返回能力位：5 键/进度条/CRC/温度） | 旧主机不发；新主机先问再决定 |
| `0x37` | 严格模式开关（CRC32 + 长度校验） | 旧主机不发，默认关闭 |

> 注意：这些都必须在 `otaWritePre()` 的 `0x10..0x44` 路由范围内才可能到达应用层；`0x45+` 或 `0x50+` 在参考固件上会被 OTA 吃掉（`ble.c:195`）。

---

## 11. 未验证事项与风险

1. **全部结论来自源码静态阅读**：没有构建、没有烧录、没有实测时序/功耗/刷新波形。`epd_bw_213.c` 的时序参数（10 ms reset、3000 ms BUSY 上限）与实际面板是否匹配未验证。
2. **面板型号**：代码注释写 `GDEQ0213B74`，README 写 SSD1680（`readme.md:131`），两者是否等价未验证；只影响驱动层，不影响本文契约。
3. **`epd_buffer_size = 4736` 与 4000 B 帧的关系**：阅读/锁屏只提交 4000 B，但数组按 296 宽分配（`epd.h:4-5`）。这是参考实现的遗留不一致；E2S 应按 250×128=4000 B 实现，不要照抄 4736。
4. **`0x33` legacy 全量流会丢包**（注释自述 3/23，`ebook_ble.c:520-528`），实测未做。E2S 实现必须用请求-应答模式。
5. **锁屏态按键延迟**：参考实现靠 1 s 广播周期唤醒轮询，理论最坏 ~1 s（`app_config.h:9`、`ebook_buttons.c:216-219` 注释写 ~100 ms，与配置不一致）。E2S 用 GPIO 唤醒后此问题消失，但不能据此推断参考实现的真实延迟。
6. **`settings` 的 CRC 是逐字节 XOR**（`flash.c:62-70`），不是真 CRC；E2S 换 NVS 时不要依赖它做完整性校验。
7. **`0x34` 的 `fw_major/minor/patch` 来自 `build_info.h`**（`app_config.h:14`、`ebook_ble.c:597-599`），具体版本号随构建变化，不属于协议常量。
8. **`web` 工具依赖 `gb2312_map.js`（105 KB）**；E2S 主机若自己实现转码，必须保证"不可映射字符 → 0xA1A1"这一行为（`web:857-868`），否则页边界会与参考不一致。
9. **周几渲染**用 ASCII 码点索引中文点阵（`epd.c:495-500`、`font16zh.h:128-173`），字模与码点的对应关系只按注释与偏移推断，未逐字验证。

---

## 附录 A：常量速查

| 名称 | 值 | 出处 |
|---|---|---|
| 画布 | 250×128（可见 y≥6） | `ebook.h:57-58`、`epd.c:456-465` |
| 每页行数 | 5 | `ebook.h:87` |
| 行高 | HZ 18 / ASCII 19 | `ebook.h:92-93` |
| 正文首行 | HZ top 30 / ASCII baseline 43 | `ebook.h:66-67` |
| 行宽 | 246 | `ebook.h:86` |
| 全角字数/行 | 15 | 16×15=240≤246 |
| 单行读取上限 | 80 B | `ebook.h:95` |
| 回退窗口 | 5×80=400 B + 前瞻 80 B | `ebook.h:100`、`ebook_layout.h:133-136` |
| 页首栈深 | 16 | `ebook.c:471` |
| 目录 | 8 × 32 B @0x00 | `ebook.h:10`、`ebook.h:35-41` |
| 标题 | 字段 20 B / 上限 19 B | `ebook.h:38`、`ebook.c:107` |
| 字库 | 261,696 B @0x001000 | `ebook.h:6`（=87×94×32） |
| 书区起点 | 0x041000 | `ebook.h:7` |
| 进度环 | 0x7FE000，8 B × 512 | `ebook.h:26-29` |
| 锁图 | 0x7FF000，4000 B，标志 @+0x0FF0 = 0xAA | `ebook.h:14-16` |
| 长按 | 1000 ms（10 × 100 ms keepalive） | `ebook_buttons.c:27-28` |
| 双击窗口 | 350 ms | `lwbtn_opts.h:22` |
| 点击上限 | 400 ms | `lwbtn_opts.h:21` |
| 休眠选项 | 15/30/60/120 s，默认 60 | `flash.c:14`、`flash.c:50` |
| GC 选项 | 10/20/50/100，默认 10 | `flash.c:15`、`flash.c:52` |
| MTU | 247（载荷 244；数据 243） | `ble.c:34`、`web:767` |

## 附录 B：本次阅读的证据文件清单

- `Firmware/src/ebook.h`、`ebook_layout.h`、`ebook.c`、`ebook_buttons.c`、`ebook_ble.c`
- `Firmware/src/app.c`、`app_config.h`、`main.c`、`flash.c/h`、`ext_flash.c`
- `Firmware/src/epd.c`、`epd.h`、`epd_refresh.c/h`、`epd_bw_213.c`、`epd_spi.c`、`epd_ble_service.c`
- `Firmware/src/ble.c`、`app_att.c`、`ota.c`、`cmd_parser.c`、`etime.c`、`buttons.h`
- `Firmware/src/lwbtn_opts.h`、`lwbtn.c`、`font16zh.h`
- 仓库根 `MiaoPaper.html`（全文 1292 行，重点 §9 所列行号）
- `web_tools/js/gb2312_codec.js`
- `readme.md`
- 目标侧：`firmware/e2s_mp/main/mp_store.h`、`mp_font.h`、`CMakeLists.txt`；`firmware/e2s_reader/components/input/include/input.h`
