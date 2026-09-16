#!/usr/bin/env python3
"""Decode the compositor profiling overlay out of a screenshot.

Turn on N64_PROFILE_OVERLAY in src/n64/gpu_regs_n64.c, run the ROM, take a
screenshot of the whole VI output, and pass it here.

Each measured value is one row band of 4 VI lines, 32 cells of 8 VI pixels,
white for 1 and dark grey for 0. The screenshot is 2x horizontally and
roughly 2x vertically (VI outputs 237 lines stretched to 480).
"""
import sys
from PIL import Image

LABELS = ["backgrounds", "sprites", "blit", "idle-between", "frames", "regs"]
COUNT_FREQ = 46875000


def decode(path):
    im = Image.open(path).convert("RGB")
    W, H = im.size
    px = im.load()
    xs = W / 320.0          # VI pixel -> screenshot pixel
    ys = H / 237.0
    out = []
    for band in range(6):
        vi_row = band * 4 + 1          # middle-ish of the 4-line band
        y = int(vi_row * ys + ys / 2)
        bits = 0
        for bit in range(32):
            vi_x = bit * 8 + 4
            x = int(vi_x * xs)
            r, g, b = px[min(x, W - 1), min(y, H - 1)]
            bits = (bits << 1) | (1 if (r + g + b) > 300 else 0)
        out.append(bits)
    return out


for path in sys.argv[1:]:
    vals = decode(path)
    print(path)
    total = sum(vals[:4])
    for label, v in zip(LABELS, vals):
        if label in ("frames", "regs"):
            print(f"  {label:<14} {v}  (hex {v:08x})")
        else:
            ms = v / COUNT_FREQ * 1000.0
            pct = 100.0 * v / total if total else 0
            print(f"  {label:<14} {v:>10} counts  {ms:7.2f} ms  {pct:5.1f}%")
    if total:
        print(f"  {'frame total':<14} {total:>10} counts  "
              f"{total / COUNT_FREQ * 1000:7.2f} ms  "
              f"{COUNT_FREQ / total:5.1f} fps")
    print()
