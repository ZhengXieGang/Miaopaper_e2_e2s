#!/usr/bin/env python3
"""Build the README screenshot sheet from simulator 1x frames.

The selected frames are arranged in a 3-column grid on a white background.
Each tile is the real 250x122 render, scaled 2x, with a short label underneath.
"""

import argparse
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

W, H = 250, 122
SCALE = 2
TILE_W, TILE_H = W * SCALE, H * SCALE

BG = (255, 255, 255)
BORDER = (176, 176, 184)
LABEL = (82, 82, 91)

CJK_CANDIDATES = [
    "/usr/share/fonts/noto-cjk/NotoSansCJK-Medium.ttc",
    "/usr/share/fonts/opentype/noto/NotoSansCJK-Medium.ttc",
    "/usr/share/fonts/truetype/noto/NotoSansCJK-Medium.ttc",
]
CJK_INDEX = 2

KEEP = [
    "clock", "select", "read_cn", "settings", "settings_scroll",
    "about", "backlight_popup", "book_info", "jump", "time_set",
    "lock_img", "lock_stats",
]

LABELS = {
    "clock": "时钟",
    "select": "书库",
    "read_cn": "阅读",
    "read_en": "英文阅读",
    "read_end": "末页",
    "settings": "设置",
    "settings_off": "蓝牙关闭",
    "settings_scroll": "设置滚动",
    "backlight": "背光",
    "backlight_popup": "快捷背光",
    "book_info": "书籍信息",
    "jump": "跳转",
    "time_set": "时间设置",
    "time_done": "设置完成",
    "delete": "删除本书",
    "about": "关于",
    "lock_img": "图片锁屏",
    "lock_stats": "阅读统计",
    "lock_none": "解锁提示",
    "no_lock_img": "未上传锁屏图",
    "empty": "空书库",
    "error": "错误",
    "restore": "恢复原厂",
    "restore_ok": "已切换",
}


def cjk_font(size: int) -> ImageFont.FreeTypeFont:
    for path in CJK_CANDIDATES:
        if Path(path).exists():
            return ImageFont.truetype(path, size, index=CJK_INDEX)
    raise SystemExit("Noto Sans CJK SC not found")


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--frames-dir", default="sim/out", type=Path)
    ap.add_argument("--out", default="docs/images/screens.png", type=Path)
    ap.add_argument("--cols", default=3, type=int)
    args = ap.parse_args()

    order_file = args.frames_dir / "order.txt"
    all_names = [line.strip() for line in order_file.read_text().splitlines()
                 if line.strip()]
    names = [name for name in KEEP if name in all_names]
    if len(names) != len(KEEP):
        missing = [name for name in KEEP if name not in all_names]
        raise SystemExit(f"{order_file}: missing {missing}")

    gap_x, gap_y = 16, 18
    margin = 32
    label_h = 28
    cell_h = TILE_H + label_h
    rows = (len(names) + args.cols - 1) // args.cols
    width = margin * 2 + args.cols * TILE_W + (args.cols - 1) * gap_x
    height = margin * 2 + rows * cell_h + (rows - 1) * gap_y

    sheet = Image.new("RGB", (width, height), BG)
    draw = ImageDraw.Draw(sheet)
    label_font = cjk_font(18)

    for i, name in enumerate(names):
        path = args.frames_dir / f"{name}_1x.png"
        if not path.exists():
            raise SystemExit(f"{path}: missing")
        im = Image.open(path).convert("L")
        if im.size != (W, H):
            raise SystemExit(f"{path}: expected {W}x{H}, got {im.size}")
        tile = im.resize((TILE_W, TILE_H), Image.Resampling.NEAREST).convert("RGB")

        col, row = i % args.cols, i // args.cols
        in_row = min(args.cols, len(names) - row * args.cols)
        x = margin + (args.cols - in_row) * (TILE_W + gap_x) // 2 + col * (TILE_W + gap_x)
        y = margin + row * (cell_h + gap_y)

        draw.rectangle((x - 1, y - 1, x + TILE_W, y + TILE_H), fill=BORDER)
        sheet.paste(tile, (x, y))

        text = LABELS.get(name, name)
        box = draw.textbbox((0, 0), text, font=label_font)
        tw = box[2] - box[0]
        draw.text((x + (TILE_W - tw) // 2, y + TILE_H + 5), text,
                  font=label_font, fill=LABEL)

    args.out.parent.mkdir(parents=True, exist_ok=True)
    sheet.save(args.out, optimize=True)
    print(f"{args.out} {sheet.width}x{sheet.height}, {len(names)} frames")


if __name__ == "__main__":
    main()
