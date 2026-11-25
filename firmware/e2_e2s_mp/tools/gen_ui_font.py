#!/usr/bin/env python3
"""gen_ui_font.py — render a TrueType face into an Adafruit-GFX C header.

The firmware's mp_font.c already understands the GFXfont/GFXglyph layout
(bit-continuous 1bpp rows, MSB first), so the only job here is to rasterise
the requested character set with Pillow and emit the two arrays.

Two fonts are checked in:
  main/fonts/font_ui16.h    Inter Regular 16 px, ASCII 0x20..0x7E
  main/fonts/font_clock.h   Inter SemiBold 66 px, digits + ':' + '%'

Both derive from Inter (SIL Open Font License 1.1).  Only the rendered
bitmaps are distributed; see docs/FONT.md / NOTICE.

Usage:
  python3 tools/gen_ui_font.py --font Inter-400.ttf --size 16 \
      --name Inter_UI_16 --out main/fonts/font_ui16.h --chars ascii
  python3 tools/gen_ui_font.py --font Inter-600.ttf --size 66 \
      --name Inter_Clock_66 --out main/fonts/font_clock.h \
      --chars '0123456789:%' --tabular-digits
"""
import argparse
import os
import sys

from PIL import Image, ImageDraw, ImageFont

THRESHOLD = 100
SUPERSAMPLE = 3


def glyph_bitmap_mono(font_path, size, ch, tabular_adv=None, center_digit_h=None):
    """使用 FreeType 原生 1bpp monochrome hinting 生成并裁切单字位图。"""
    font = ImageFont.truetype(font_path, size)
    advance = round(font.getlength(ch))
    mask, (xoff, yoff) = font.getmask2(ch, mode="1", anchor="ls")
    mw, mh = mask.size

    # 扫描有效墨迹边界，裁掉四周全 0 的空行与空列
    min_x, min_y = mw, mh
    max_x, max_y = -1, -1
    for y in range(mh):
        for x in range(mw):
            if mask.getpixel((x, y)):
                if x < min_x:
                    min_x = x
                if x > max_x:
                    max_x = x
                if y < min_y:
                    min_y = y
                if y > max_y:
                    max_y = y

    if max_x < min_x or max_y < min_y:
        if tabular_adv is not None:
            advance = tabular_adv
        return advance, 0, 0, 0, 0, b""

    w = max_x - min_x + 1
    h = max_y - min_y + 1
    xoff += min_x
    yoff += min_y
    if tabular_adv is not None:
        # 等宽数字在槽位内水平居中
        advance = tabular_adv
        xoff = (advance - w) // 2
    elif center_digit_h is not None and ch in (":", "-"):
        # 分隔符按数字视觉高度垂直居中，并保留对称左右留白
        yoff = -((center_digit_h + h) // 2)
        xoff = 3
        advance = w + 6
    elif ch == ":":
        # 冒号在 12 px 数字中上移 1 px 以实现垂直居中
        yoff -= 1

    total_bits = w * h
    nbytes = (total_bits + 7) // 8
    packed = bytearray(nbytes)
    bitpos = 0
    for y in range(min_y, max_y + 1):
        for x in range(min_x, max_x + 1):
            if mask.getpixel((x, y)):
                packed[bitpos >> 3] |= 0x80 >> (bitpos & 7)
            bitpos += 1
    return advance, xoff, yoff, w, h, bytes(packed)


def glyph_bitmap(font_path, size, ch, tabular_adv=None, colon_digit_h=None):
    """Return (advance, xoff, yoff, w, h, rows) for one character."""
    font = ImageFont.truetype(font_path, size)
    scale_font = ImageFont.truetype(font_path, size * SUPERSAMPLE)

    # Pillow's 'ls' anchor puts (0, 0) on the baseline.  bbox is therefore
    # already relative to the baseline, which is exactly GFXglyph's contract.
    probe = Image.new("L", (1, 1), 0)
    draw = ImageDraw.Draw(probe)
    bx0, by0, bx1, by1 = draw.textbbox((0, 0), ch, font=scale_font, anchor="ls")
    w3 = max(1, bx1 - bx0)
    h3 = max(1, by1 - by0)
    img = Image.new("L", (w3, h3), 0)
    ImageDraw.Draw(img).text((-bx0, -by0), ch, font=scale_font, fill=255,
                             anchor="ls")
    img = img.resize((max(1, round(w3 / SUPERSAMPLE)),
                      max(1, round(h3 / SUPERSAMPLE))), Image.LANCZOS)

    w, h = img.size
    rows = []
    for y in range(h):
        bits = 0
        for x in range(w):
            if img.getpixel((x, y)) >= THRESHOLD:
                bits |= 1 << (w - 1 - x)
        rows.append(bits)

    advance = round(font.getlength(ch))
    xoff = round(bx0 / SUPERSAMPLE)
    yoff = round(by0 / SUPERSAMPLE)
    if tabular_adv is not None:
        advance = tabular_adv
        xoff = (advance - w) // 2
    elif colon_digit_h is not None and ch == ":":
        # 大号时钟冒号垂直居中于数字高度，并设置对称左右留白
        yoff = -((colon_digit_h + h) // 2)
        xoff = 3
        advance = w + 6

    # Pack bit-continuously, exactly like the existing font headers.
    total_bits = w * h
    nbytes = (total_bits + 7) // 8
    packed = bytearray(nbytes)
    bitpos = 0
    for y in range(h):
        row = rows[y]
        for x in range(w):
            if row & (1 << (w - 1 - x)):
                packed[bitpos >> 3] |= 0x80 >> (bitpos & 7)
            bitpos += 1
    return advance, xoff, yoff, w, h, bytes(packed)


def c_array(name, data, per_line=16):
    out = ["const uint8_t %s[] PROGMEM = {" % name]
    for i in range(0, len(data), per_line):
        chunk = ",".join("0x%02X" % b for b in data[i:i + per_line])
        out.append("    %s," % chunk)
    out.append("};")
    return "\n".join(out)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--font", required=True, help="TrueType/OpenType file")
    ap.add_argument("--size", type=int, required=True)
    ap.add_argument("--name", required=True, help="C identifier prefix")
    ap.add_argument("--out", required=True)
    ap.add_argument("--chars", default="ascii",
                    help="'ascii' or an explicit character list")
    ap.add_argument("--tabular-digits", action="store_true",
                    help="give 0-9 one common advance (clock use)")
    ap.add_argument("--mono", action="store_true",
                    help="使用 FreeType 1bpp monochrome hinting（小字号 UI 字体）")
    ap.add_argument("--comment", default="", help="source/licence note")
    args = ap.parse_args()

    if not os.path.isfile(args.font):
        sys.exit("gen_ui_font: font not found: %s" % args.font)

    chars = ([chr(c) for c in range(0x20, 0x7F)] if args.chars == "ascii"
             else sorted(set(args.chars), key=ord))
    if not chars:
        sys.exit("gen_ui_font: empty character set")

    first = ord(chars[0])
    last = ord(chars[-1])
    wanted = set(chars)

    # For a tabular clock face, measure the widest digit once.
    tabular = None
    colon_digit_h = None
    if args.tabular_digits:
        f = ImageFont.truetype(args.font, args.size)
        tabular = max(round(f.getlength(c)) for c in "0123456789")
        if args.mono:
            # 对单色点阵采样 0..9 宽高，确保宽字形至少保留 1 px 左右留白
            samples = [glyph_bitmap_mono(args.font, args.size, c)
                       for c in "0123456789"]
            max_w = max(s[3] for s in samples)
            tabular = max(tabular, max_w + 2)
            colon_digit_h = round(sum(s[4] for s in samples) / len(samples))
        else:
            # 统计 0..9 的平均视觉高度用于冒号垂直居中
            hs = [glyph_bitmap(args.font, args.size, c, tabular)[4]
                  for c in "0123456789"]
            colon_digit_h = round(sum(hs) / len(hs))

    bitmap = bytearray()
    glyphs = []
    offset = 0
    for code in range(first, last + 1):
        ch = chr(code)
        if ch not in wanted:
            # GFXfont requires a dense range.  Unused slots draw nothing.
            glyphs.append((offset, 0, 0, 0, 0, 0))
            continue
        if args.mono:
            advance, xoff, yoff, w, h, packed = glyph_bitmap_mono(
                args.font, args.size, ch,
                tabular if (args.tabular_digits and ch.isdigit()) else None,
                colon_digit_h if args.tabular_digits else None)
        else:
            advance, xoff, yoff, w, h, packed = glyph_bitmap(
                args.font, args.size, ch,
                tabular if (args.tabular_digits and ch.isdigit()) else None,
                colon_digit_h)
        glyphs.append((offset, w, h, advance, xoff, yoff))
        bitmap.extend(packed)
        offset += len(packed)

    lines = []
    lines.append("/* Generated by tools/gen_ui_font.py — do not edit by hand.")
    if args.comment:
        lines.append(" * %s" % args.comment)
    lines.append(" * Font: %s @ %d px (SIL Open Font License 1.1)."
                 % (os.path.basename(args.font), args.size))
    lines.append(" */")
    lines.append("#pragma once")
    lines.append("")
    lines.append(c_array(args.name + "Bitmaps", bitmap))
    lines.append("")
    lines.append("const GFXglyph %sGlyphs[] PROGMEM = {" % args.name)
    for (code, (off, w, h, adv, xo, yo)) in zip(range(first, last + 1), glyphs):
        lines.append("  { %5d, %3d, %3d, %3d, %4d, %4d }, // '%s'"
                     % (off, w, h, adv, xo, yo,
                        chr(code) if 0x20 <= code < 0x7F else "?"))
    lines.append("};")
    lines.append("")
    # mp_font.c follows the legacy tables, whose declared `last` is one past
    # the final stored glyph (the old headers stop at 0x7D but declare 0x7E).
    # Emit the same convention so the renderer's last = first + count includes
    # our real final character (':' for the clock, '~' for the UI face).
    font_last = last + 1
    lines.append("const GFXfont %s = {" % args.name)
    lines.append("(uint8_t *)%sBitmaps, (GFXglyph *)%sGlyphs, 0x%02X, 0x%02X, %d};"
                 % (args.name, args.name, first, font_last, args.size))
    lines.append("")

    with open(args.out, "w", encoding="utf-8") as f:
        f.write("\n".join(lines))
    print("wrote %s: %d glyphs, %d bitmap bytes" % (args.out, len(chars),
                                                    len(bitmap)))


if __name__ == "__main__":
    main()
