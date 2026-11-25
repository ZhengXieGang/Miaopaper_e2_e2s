# main/fonts — 字模来源与许可

| 文件 | 用途 | 来源 / 许可 |
|---|---|---|
| `font16.h` | 16 px ASCII / 中文点阵（Dialog 16 系，含 `_zh` 变体） | 从 Miaopaper 内部构建复制；上游仓库 MIT，但字模本身未单独声明许可。**只限内部构建，对外分发前复核**。`_zh` 已不参与编译。 |
| `font30.h` | Special Elite 30 | 同上；**已不参与编译**。 |
| `font_60.h` | DSEG14 40 px 数码管 | 同上；UI v2 已用 Inter 替换，**已不参与编译**。 |
| `font_ui16.h` | UI ASCII（Inter Regular 16 px） | 由 `tools/gen_ui_font.py` 从 **Inter 4.x** 生成。Inter 由 The Inter Project Authors 发布，**SIL Open Font License 1.1**（https://github.com/rsms/inter 、 https://openfontlicense.org）。本文件只包含光栅化后的 1bpp 点阵。 |
| `font_clock.h` | 大号时钟/百分比（Inter SemiBold 66 px，等宽数字） | 同上，SIL OFL 1.1。 |

## 重新生成

```sh
cd firmware/e2_e2s_mp
python3 tools/gen_ui_font.py --font Inter-400.ttf --size 16 \
    --name Inter_UI_16 --out main/fonts/font_ui16.h --chars ascii
python3 tools/gen_ui_font.py --font Inter-600.ttf --size 66 \
    --name Inter_Clock_66 --out main/fonts/font_clock.h \
    --chars '0123456789:%' --tabular-digits
```

`--chars ascii` 覆盖 0x20..0x7E；时钟表覆盖 `%`、`0-9`、`:`。
生成器把 `GFXfont.last` 写成“最后一个字形 + 1”，与旧表（`last=0x7E`
但只存到 0x7D）的约定一致，渲染器不会读表外。

## HZK16

`components/text/assets/HZK16_87.bin` 与生成的 `hz16_gb2312.bin` 只限内部；
对外发行前按 `components/text/assets/NOTICE.md` 用 OFL/Apache-2.0 的 TTF 重新生成。
