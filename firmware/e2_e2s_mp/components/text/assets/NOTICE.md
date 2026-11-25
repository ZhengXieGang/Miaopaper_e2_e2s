HZK16 16x16 点阵字库 / GB2312 映射 — 资产来源与许可说明
=========================================================

本目录(text 工作包)有三份二进制资产 + 一份 ASCII 点阵源文件。运行时的两份
资产由 `components/text/CMakeLists.txt` 以 `EMBED_FILES` 嵌入 app 镜像;固件
不读 SD 卡字库,也不写原厂字库分区(INVENTORY §2.4/§7 #11)。

文件一览
--------
| 文件 | 大小 | 作用 | MD5 | SHA-256 |
|---|---|---|---|---|
| hz16_gb2312.bin | 261,696 B | 16x16 点阵(87 区 x 94 位 x 32 B) | 86b1d03e27802a485207abee7d3b13a5 | 675f3d39c67ec4d9b7192b581fd589ab8367bac2d89a90f59695c4383d54602d |
| gb2312_map.bin | 29,792 B | 12 B 头 + 7,445 条 (u16 Unicode, u16 区位槽) | 383d0c97ad847225b534a94b59ab4d36 | 2f6a86312594565f764888dd46f59a58985e5b1769808fe3633f177b44da95b2 |
| HZK16_87.bin | 261,696 B | 内部构建输入(不随发行物分发) | 86b1d03e27802a485207abee7d3b13a5 | 675f3d39c67ec4d9b7192b581fd589ab8367bac2d89a90f59695c4383d54602d |

格式、寻址公式、生成器用法与全部校验值见 `docs/FONT.md`。

格式摘要
--------
hz16_gb2312.bin:
  区位 (hi,lo) -> 槽号 (hi-0xA1)*94 + (lo-0xA1) -> 偏移 槽号*32
  每个字模 32 B:16 行 x 2 B 大端,MSB 最左,bit=1 墨。
  实测:0xB0A1(啊)偏移 45,120,前 8 B = 00 04 2f 7e f9 04 a9 04;
        0xA1A1(全角空格)32 B 全 0;未分配槽位 733 个(渲染为 16 px 空心框)。

gb2312_map.bin:
  0..3 "GBM1" | 4..5 u16 count=7445 | 6..7 u16 version=1 |
  8..11 u32 crc32(条目区,当前 0xd09c0bd2) | 12.. 7445 x (u16 Unicode, u16 slot),
  按 Unicode 严格升序。只做 Unicode->区位 的正向查询。

生成(可重复)
-------------
  python3 tools/gen_gb2312_map.py
  python3 tools/gen_hz16.py --from-hzk16        # 内部版(本目录当前内容)
  python3 tools/gen_hz16.py --check             # 校验大小/向量
  python3 tools/gen_gb2312_map.py --check
  tools/run_host_tests.sh                       # 逐项断言

许可
----
- HZK16_87.bin 复制自 refs/Miaopaper/HZK16_87.bin(Miaopaper 仓库 MIT,
  commit 1ef51c01778651030b012840149e84e55307cdc9),但**字模本身的许可不明**
  (INVENTORY §4.8 / DESIGN §5.3/§11 #10):MIT 只覆盖作者原创的代码与资产,
  不能推定 HZK16 字模可再分发。
- 因此 HZK16_87.bin 与由它生成的 hz16_gb2312.bin 只作为**内部构建输入/自用**,
  不随对外发行物分发。
- **注意区分"不分发输入文件"和"固件里没有字模"**:默认的内部构建会把
  hz16_gb2312.bin 以 `EMBED_FILES` 嵌进 app 镜像,而它当前与 HZK16_87.bin
  **逐字节相同**(MD5 `86b1d03e27802a485207abee7d3b13a5`)。也就是说,按当前
  assets/ 构建出的固件**包含** HZK16 的逐字节副本。对外发行固件(而不只是
  发行源码)前,必须先用 `--from-ttf` 重新生成 hz16_gb2312.bin。
- 对外分发前必须用 `tools/gen_hz16.py --from-ttf <允许再分发的 TTF>` 重新生成
  hz16_gb2312.bin(推荐 OFL-1.1 或 Apache-2.0、TrueType glyf 轮廓、Regular 及以上
  字重的 CJK 字体),并在发行物中保留该字体的许可文本。已实测
  DroidSansFallbackFull.ttf(Apache-2.0)可用;Noto Sans CJK 的 .ttc 是 CFF
  轮廓,生成器会明确拒绝。
- gb2312_map.bin 是 GB2312 码位映射(事实数据),可随固件分发。
- font8x16.c 复制自 firmware/badapple/main/font.c,来源头已保留;点阵许可未单独
  声明,分发前需复核。
- 不要把 HZK16 系资产写到设备 0x480000–0x528884 的原厂字库区。
