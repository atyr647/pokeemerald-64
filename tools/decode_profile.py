#!/usr/bin/env python3
"""Decode the compositor profiling overlay out of an emulator screenshot.

Turn on N64_PROFILE_OVERLAY in src/n64/gpu_regs_n64.c and screenshot the
emulator. The overlay paints a full-width white marker across the top two
lines of the VI framebuffer and then one 4-line band per value, each band
32 cells of 8 VI pixels, white for a 1 bit.

The marker is what makes this work on any emulator: its ends are VI x=0 and
x=319, so finding it gives the horizontal scale and origin, and its top row
gives the vertical origin. Emulators letterbox and scale differently --
mupen64plus fills its window, ares centres the picture under a menu bar --
and neither has to be special-cased.
"""
import sys
from PIL import Image

COUNT_FREQ = 46875000          # CP0 Count ticks per second on a VR4300

# Must match the OV_* constants in src/n64/gpu_regs_n64.c.
OV_X0 = 16
OV_CELL = 8
OV_CELLS = 33
OV_MARK_ROW = 2
OV_MARK_ROWS = 4
OV_BAND_ROW = 8
OV_BAND_ROWS = 4
OV_BITS = 32


def find_marker(px, W, H):
    """Return (x0, x1, y_marker) of the white bar in screenshot pixels."""
    best = None
    for y in range(H):
        run_start = None
        for x in range(W + 1):
            white = False
            if x < W:
                r, g, b = px[x, y][:3]
                white = r > 200 and g > 200 and b > 200
            if white and run_start is None:
                run_start = x
            elif not white and run_start is not None:
                length = x - run_start
                if best is None or length > best[0]:
                    best = (length, run_start, x - 1, y)
                run_start = None
    if best is None or best[0] < OV_CELLS:
        return None
    return best[1], best[2], best[3]


def decode(path, count):
    im = Image.open(path).convert("RGB")
    W, H = im.size
    px = im.load()

    found = find_marker(px, W, H)
    if not found:
        raise SystemExit(f"{path}: no marker found — is the overlay enabled?")
    x0, x1, ymark = found

    cell_w = (x1 - x0 + 1) / OV_CELLS
    # The marker is the top two lines of a four-line band, so a band is four
    # VI lines below the marker's first line. Vertical scale is taken from the
    # horizontal one only when the emulator preserves aspect; instead, find
    # the marker's own height to measure it directly.
    # Each band paints a white stripe in a column left of the cells, so the
    # bands are found by looking for stripes rather than by scaling. ares
    # corrects the N64's pixel aspect and mupen64plus does not, so a vertical
    # scale inferred from the horizontal one drifts a band out by the bottom
    # of the overlay.
    ruler_x = int(x0 + cell_w / 2)
    ruler_x = max(0, min(W - 1, ruler_x))

    # The marker spans every cell, including the ruler's, so skip past it
    # before collecting stripes -- otherwise the marker reads as band 0 and
    # every value comes out shifted by one.
    y = ymark
    while y < H:
        r, g, b = px[ruler_x, y][:3]
        if not (r > 200 and g > 200 and b > 200):
            break
        y += 1

    bands = []
    while y < H:
        r, g, b = px[ruler_x, y][:3]
        if r > 200 and g > 200 and b > 200:
            start = y
            while y + 1 < H:
                r, g, b = px[ruler_x, y + 1][:3]
                if not (r > 200 and g > 200 and b > 200):
                    break
                y += 1
            bands.append((start + y) // 2)
        y += 1

    values = []
    for band in range(count):
        if band >= len(bands):
            values.append(None)
            continue
        y = bands[band]
        bits = 0
        for bit in range(OV_BITS):
            x = int(x0 + (1 + bit + 0.5) * cell_w)
            r, g, b = px[min(x, W - 1), y][:3]
            bits = (bits << 1) | (1 if (r + g + b) > 300 else 0)
        values.append(bits)
    return values


def main():
    labels = ["backgrounds", "sprites", "blit", "idle", "frames", "regs"]
    if "--labels" in sys.argv:
        i = sys.argv.index("--labels")
        labels = sys.argv[i + 1].split(",")
        del sys.argv[i:i + 2]

    for path in sys.argv[1:]:
        vals = decode(path, len(labels))
        print(path)
        timed = [v for lab, v in zip(labels, vals)
                 if lab in ("backgrounds", "sprites", "blit", "idle") and v]
        total = sum(timed)
        for lab, v in zip(labels, vals):
            if v is None:
                print(f"  {lab:<14} --")
            elif lab in ("backgrounds", "sprites", "blit", "idle"):
                pct = 100.0 * v / total if total else 0
                print(f"  {lab:<14} {v:>10} counts  "
                      f"{v / COUNT_FREQ * 1000:7.2f} ms  {pct:5.1f}%")
            else:
                print(f"  {lab:<14} {v}  (hex {v:08x})")
        if total:
            print(f"  {'frame total':<14} {total:>10} counts  "
                  f"{total / COUNT_FREQ * 1000:7.2f} ms  "
                  f"{COUNT_FREQ / total:5.1f} fps")
        print()


if __name__ == "__main__":
    main()
