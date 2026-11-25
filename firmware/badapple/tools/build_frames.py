#!/usr/bin/env python3
"""Build frames.bin for the E2S Bad Apple firmware.

Source: /tmp/badapple/frames_wide.raw (6 fps, 162x122 gray, from the 480p video).
Output: 438 frames at 2 fps, 250x122 1bpp, row stride 32, 1=white, RLE.
"""
import argparse
import numpy as np, struct, os

W, H = 250, 122
ROW = 32
FB = ROW * H
OUT = 'main/frames.bin'
SRC = 'frames_wide.raw'
FPS = 2.0
SRC_FPS = 6.0

def varint(n):
    out = bytearray()
    while True:
        b = n & 0x7f
        n >>= 7
        if n:
            out.append(b | 0x80)
        else:
            out.append(b)
            return bytes(out)

def rle_encode(bits):
    """bits: 1-D uint8 0/1. Runs alternate, starting with 0."""
    changes = np.flatnonzero(bits[1:] != bits[:-1]) + 1
    bounds = np.concatenate(([0], changes, [len(bits)]))
    runs = np.diff(bounds)
    out = bytearray()
    if bits[0] == 1:
        out += varint(0)  # empty black run
    for r in runs:
        out += varint(int(r))
    return bytes(out)

def decode(bits_len, data):
    bits = np.zeros(bits_len, np.uint8)
    pos = 0
    val = 0
    i = 0
    while i < len(data):
        shift = 0
        n = 0
        while True:
            b = data[i]; i += 1
            n |= (b & 0x7f) << shift
            if not (b & 0x80):
                break
            shift += 7
        bits[pos:pos+n] = val
        pos += n
        val ^= 1
        if pos > bits_len:
            raise ValueError('run overflow')
    if pos != bits_len:
        raise ValueError(f'short frame {pos} != {bits_len}')
    return np.packbits(bits, bitorder='big')

def main():
    a = np.fromfile(SRC, dtype=np.uint8)
    n = len(a) // (122 * 162)
    a = a[:n * 122 * 162].reshape(n, 122, 162)
    step = SRC_FPS / FPS
    count = int(n * FPS / SRC_FPS)
    idx = [min(int(round(i * step)), n - 1) for i in range(count)]

    frames = []
    for k, si in enumerate(idx):
        g = a[si]
        white = (g >= 128)
        canvas = np.zeros((H, W), np.uint8)  # fixed black letterbox bars
        canvas[:, 44:44 + 162] = np.where(white, 255, 0)
        frames.append(np.packbits(canvas, axis=1, bitorder='big'))

    # Full-refresh policy: only frame 0, to initialize the panel and RAM2.
    # Everything else is partial refresh; ghosting can be cleared on demand
    # with the firmware's (1,242) command.
    full = [False] * count
    full[0] = True

    payload = bytearray()
    offsets = []
    for f in frames:
        offsets.append(len(payload))
        bits = np.unpackbits(f, bitorder='big')
        payload += rle_encode(bits)

    header = struct.pack('<4sBBHHHI', b'BAPL', 1, 0, int(FPS), W, H, count)
    table = bytearray()
    for off, fl in zip(offsets, full):
        table += struct.pack('<IB3x', off, 1 if fl else 0)
    blob = header + bytes(table) + bytes(payload)
    open(OUT, 'wb').write(blob)

    # Verify every frame decodes byte-exactly.
    table_start = len(header)
    for k in range(count):
        off, fl = struct.unpack_from('<IB', blob, table_start + 8 * k)
        end = struct.unpack_from('<I', blob, table_start + 8 * (k + 1))[0] if k + 1 < count else len(payload)
        if end <= off:
            raise ValueError(f'bad offset at {k}')
        dec = decode(FB * 8, blob[table_start + len(table) + off: table_start + len(table) + end])
        if not np.array_equal(dec.reshape(H, ROW), frames[k]):
            raise ValueError(f'frame {k} mismatch')
    print(f'frames={count} full={sum(full)} payload={len(payload)} total={len(blob)}')
    print(f'sha256={__import__("hashlib").sha256(blob).hexdigest()}')
    print(f'avg={len(payload)/count:.0f} B/frame')

if __name__ == '__main__':
    ap = argparse.ArgumentParser(description='Build frames.bin from 6 fps 162x122 grayscale frames')
    ap.add_argument('src', nargs='?', default=SRC)
    ap.add_argument('out', nargs='?', default=OUT)
    a = ap.parse_args()
    SRC, OUT = a.src, a.out
    main()
