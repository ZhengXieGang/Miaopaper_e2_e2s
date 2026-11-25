#!/usr/bin/env python3
"""make_contact.py — PGM frames -> 3x PNG tiles + 2-column contact sheet.

Reads the raw P5 frames written by sim_mp (<out>/pgm/NAME.pgm), upscales them
3x with NEAREST (no smoothing: 1bpp pixels stay square), adds a title bar with
the fixture name, and assembles a 2-column contact sheet.

Outputs, all inside --out:
    NAME.png        3x frame + title bar (one per fixture)
    NAME_1x.png     native 250x122 frame (pixel-exact review)
    contact_sheet.png

Only Pillow is required; the PGM parser is local so the script never depends
on ImageMagick or on Pillow's optional PGM plugin.
"""
import argparse
import math
import os
import sys

try:
    from PIL import Image, ImageDraw, ImageFont
except ImportError:  # pragma: no cover - environment guard
    sys.stderr.write(
        "make_contact.py: Pillow is not installed.\n"
        "  pip install Pillow    # or use your distro package python-pillow\n"
    )
    sys.exit(2)

SCALE = 3
TITLE_H = 28
GAP = 20
MARGIN = 20
COLS = 2
HEADER_H = 34

try:  # Pillow >= 9.1
    NEAREST = Image.Resampling.NEAREST
except AttributeError:  # pragma: no cover - old Pillow
    NEAREST = Image.NEAREST

DEFAULT_ORDER = [
    "clock", "select", "read_cn", "read_en", "read_end",
    "settings", "settings_off", "settings_scroll",
    "book_info", "jump", "time_set", "time_done", "delete",
    "about", "lock_img", "lock_none", "empty", "error", "restore",
    "restore_ok",
]


def read_pgm(path):
    """Parse a binary P5 PGM (maxval 255) without external dependencies."""
    with open(path, "rb") as f:
        data = f.read()
    pos = 0
    n = len(data)

    def token():
        nonlocal pos
        while pos < n:
            c = data[pos]
            if c in b" \t\r\n":
                pos += 1
            elif c == ord("#"):
                while pos < n and data[pos] not in b"\r\n":
                    pos += 1
            else:
                break
        start = pos
        while pos < n and data[pos] not in b" \t\r\n":
            pos += 1
        return data[start:pos]

    magic = token()
    if magic != b"P5":
        raise ValueError(f"{path}: not a binary PGM (magic={magic!r})")
    try:
        w = int(token())
        h = int(token())
        maxval = int(token())
    except ValueError as exc:
        raise ValueError(f"{path}: malformed PGM header") from exc
    if maxval != 255:
        raise ValueError(f"{path}: maxval {maxval} != 255")
    if pos < n and data[pos] in b" \t\r\n":
        pos += 1
    pixels = data[pos:]
    if len(pixels) != w * h:
        raise ValueError(
            f"{path}: {len(pixels)} pixel bytes for {w}x{h} (want {w * h})"
        )
    return Image.frombytes("L", (w, h), pixels)


def load_font(size):
    try:
        return ImageFont.load_default(size=size)
    except TypeError:  # Pillow < 10.1
        return ImageFont.load_default()


def make_tile(img, label, font):
    tw, th = img.width * SCALE, img.height * SCALE
    out = Image.new("L", (tw, th + TITLE_H), 255)
    out.paste(img.resize((tw, th), NEAREST), (0, TITLE_H))
    draw = ImageDraw.Draw(out)
    draw.line([(0, TITLE_H - 1), (tw - 1, TITLE_H - 1)], fill=0)
    draw.text((6, 5), label, fill=0, font=font)
    return out


def read_order(out_dir):
    order_path = os.path.join(out_dir, "order.txt")
    if os.path.isfile(order_path):
        with open(order_path, "r", encoding="utf-8") as f:
            names = [line.strip() for line in f if line.strip()]
        if names:
            return names
    return list(DEFAULT_ORDER)


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--out", required=True, help="directory containing pgm/")
    ap.add_argument("--only", help="only this fixture (also skips the sheet)")
    ap.add_argument("--tag", default="", help="subtitle shown on the sheet")
    ap.add_argument("--no-sheet", action="store_true")
    args = ap.parse_args()

    out_dir = args.out
    pgm_dir = os.path.join(out_dir, "pgm")
    if not os.path.isdir(pgm_dir):
        sys.stderr.write(f"make_contact.py: no frames at {pgm_dir}\n")
        return 2

    font = load_font(18)
    font_small = load_font(14)

    if args.only:
        names = [args.only]
    else:
        names = read_order(out_dir)

    frames = []
    missing = []
    for name in names:
        path = os.path.join(pgm_dir, name + ".pgm")
        if not os.path.isfile(path):
            missing.append(name)
            continue
        try:
            img = read_pgm(path)
        except (OSError, ValueError) as exc:
            sys.stderr.write(f"make_contact.py: {exc}\n")
            return 2
        frames.append((name, img))

    if missing:
        sys.stderr.write(
            "make_contact.py: missing PGM(s): " + ", ".join(missing) + "\n"
        )
        return 2
    if not frames:
        sys.stderr.write("make_contact.py: no frames to render\n")
        return 2

    tiles = []
    for name, img in frames:
        label = f"{name}  {img.width}x{img.height}"
        tile = make_tile(img, label, font)
        tiles.append(tile)
        # 1x pixel-exact copy for visual review of individual pixels.
        img.save(os.path.join(out_dir, name + "_1x.png"))
        tile.save(os.path.join(out_dir, name + ".png"))
        print(f"png: {name}.png ({tile.width}x{tile.height})")

    if args.only or args.no_sheet:
        print(f"png: {len(frames)} tile(s) in {out_dir}")
        return 0

    cols = COLS
    rows = int(math.ceil(len(tiles) / cols))
    cw = max(t.width for t in tiles)
    ch = max(t.height for t in tiles)
    sheet_w = MARGIN * 2 + cols * cw + (cols - 1) * GAP
    sheet_h = MARGIN * 2 + HEADER_H + rows * ch + (rows - 1) * GAP

    sheet = Image.new("L", (sheet_w, sheet_h), 255)
    draw = ImageDraw.Draw(sheet)
    title = f"e2s_mp simulator - {len(tiles)} frames @ {SCALE}x"
    if args.tag:
        title += f" - {args.tag}"
    draw.text((MARGIN, 9), title, fill=0, font=font_small)
    note = "1bpp: bit1=white"
    note_w = int(draw.textlength(note, font=font_small))
    draw.text((sheet_w - MARGIN - note_w, 9), note, fill=0, font=font_small)
    draw.line([(MARGIN, MARGIN + HEADER_H - 8),
               (sheet_w - MARGIN, MARGIN + HEADER_H - 8)], fill=0)

    y0 = MARGIN + HEADER_H
    for idx, tile in enumerate(tiles):
        row, col = divmod(idx, cols)
        x = MARGIN + col * (cw + GAP)
        y = y0 + row * (ch + GAP)
        sheet.paste(tile, (x, y))
        draw.rectangle([x, y, x + tile.width - 1, y + tile.height - 1],
                       outline=0)

    sheet_path = os.path.join(out_dir, "contact_sheet.png")
    sheet.save(sheet_path)
    print(f"sheet: {sheet_path} ({sheet.width}x{sheet.height}, "
          f"{len(tiles)} frames, {cols} columns)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
