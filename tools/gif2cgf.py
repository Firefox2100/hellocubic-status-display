#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Convert an animated GIF (or APNG/WebP) to the cube's CGF1 format (see include/display/cgf.h).

The cube plays CGF1 with ~1.5 KB of RAM; a GIF needs a 24 KB decoder it can no longer spare.

    tools/gif2cgf.py in.gif out.cgf [--max 240] [--size WxH] [--colors 255] [--dump-canvas frames.raw]

The picture is shrunk (never enlarged) to fit 240x240 unless --size is given, a single 255-colour palette is built for
the whole animation, and every frame after the first only stores the rectangle that changed.
Needs Pillow and numpy.  Upload the result like a GIF:  POST /api/v1/gif (multipart file field).
"""
import argparse
import struct
import sys

import numpy as np
from PIL import Image, ImageSequence

UNCHANGED = 255
MAX_DIM = 240
MIN_RUN = 3


def rgb565(r, g, b):
    return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)


def rle(data):
    """PackBits-style: c<0x80 -> c+1 literals follow; c>=0x80 -> next byte repeated (c&0x7f)+1 times."""
    out = bytearray()
    n = len(data)
    i = 0
    lit_start = 0

    def flush(end):
        nonlocal lit_start
        while lit_start < end:
            take = min(128, end - lit_start)
            out.append(take - 1)
            out.extend(data[lit_start:lit_start + take])
            lit_start += take

    while i < n:
        j = i
        while j < n and data[j] == data[i] and j - i < 128:
            j += 1
        if j - i >= MIN_RUN:
            flush(i)
            out.append(0x80 | (j - i - 1))
            out.append(data[i])
            i = j
            lit_start = i
        else:
            i += 1
    flush(n)
    return bytes(out)


def load_frames(path, size):
    im = Image.open(path)
    frames, durations = [], []
    for frame in ImageSequence.Iterator(im):
        rgba = frame.convert("RGBA")
        base = Image.new("RGB", rgba.size, (0, 0, 0))
        base.paste(rgba, mask=rgba.split()[3])  # transparency becomes black
        if size and base.size != size:
            base = base.resize(size, Image.LANCZOS)
        frames.append(base)
        durations.append(int(frame.info.get("duration", im.info.get("duration", 100)) or 100))
    return frames, durations


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("src")
    ap.add_argument("dst")
    ap.add_argument("--max", type=int, default=MAX_DIM, help="largest width/height (default 240)")
    ap.add_argument("--size", help="force WxH instead of fitting")
    ap.add_argument("--colors", type=int, default=255, help="palette size, at most 255")
    ap.add_argument("--dump-canvas", help="write what the player must show after each frame (RGB565 LE), for tests")
    args = ap.parse_args()
    colors = max(2, min(255, args.colors))

    probe = Image.open(args.src)
    w, h = probe.size
    if args.size:
        w, h = (int(v) for v in args.size.lower().split("x"))
    elif max(w, h) > args.max:
        scale = args.max / max(w, h)
        w, h = max(1, round(w * scale)), max(1, round(h * scale))
    if w > MAX_DIM or h > MAX_DIM:
        sys.exit("size must be at most 240x240")
    frames, durations = load_frames(args.src, (w, h))

    # One palette for the whole animation: quantize a strip made of every frame.
    strip = Image.new("RGB", (w, h * len(frames)))
    for k, frame in enumerate(frames):
        strip.paste(frame, (0, k * h))
    pal_img = strip.quantize(colors=colors, method=Image.Quantize.MEDIANCUT, dither=Image.Dither.NONE)
    raw_pal = pal_img.getpalette()[: colors * 3]
    used = len(raw_pal) // 3
    palette565 = [rgb565(*raw_pal[i * 3:i * 3 + 3]) for i in range(used)]

    canvas = None
    body = bytearray()
    total_changed = 0
    for k, frame in enumerate(frames):
        idx = np.asarray(frame.quantize(palette=pal_img, dither=Image.Dither.NONE), dtype=np.uint8)
        idx = np.minimum(idx, used - 1)
        dur = max(1, min(65535, durations[k]))
        if canvas is None:
            x0, y0, x1, y1 = 0, 0, w, h
            rect = idx
        else:
            diff = idx != canvas
            if not diff.any():
                body += struct.pack("<HHHHHI", dur, 0, 0, 0, 0, 0)
                continue
            rows = np.where(diff.any(axis=1))[0]
            cols = np.where(diff.any(axis=0))[0]
            y0, y1, x0, x1 = rows[0], rows[-1] + 1, cols[0], cols[-1] + 1
            rect = np.where(diff[y0:y1, x0:x1], idx[y0:y1, x0:x1], UNCHANGED).astype(np.uint8)
        data = rle(rect.tobytes())
        body += struct.pack("<HHHHHI", dur, int(x0), int(y0), int(x1 - x0), int(y1 - y0), len(data)) + data
        total_changed += int((x1 - x0) * (y1 - y0))
        canvas = idx
    if args.dump_canvas:  # what the player must show after each frame, for the host test
        with open(args.dump_canvas, "wb") as out:
            for frame in frames:
                cur = np.asarray(frame.quantize(palette=pal_img, dither=Image.Dither.NONE), dtype=np.uint8)
                out.write(np.array(palette565, dtype="<u2")[np.minimum(cur, used - 1)].tobytes())

    header = b"CGF1" + struct.pack("<HHHBB", w, h, len(frames), used, 0)
    blob = header + b"".join(struct.pack("<H", c) for c in palette565) + bytes(body)
    with open(args.dst, "wb") as f:
        f.write(blob)
    print(f"{args.dst}: {w}x{h}, {len(frames)} frames, {used} colours, {len(blob)} bytes "
          f"(changed pixels {total_changed}, {100 * total_changed // max(1, w * h * len(frames))}% of full)")


if __name__ == "__main__":
    main()
