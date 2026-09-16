#!/usr/bin/env python3
"""tools/n64_wavedata_be.py <sample.bin>

Byte-swap a wav2agb sample's header for a big-endian target.

wav2agb writes struct WaveData for the GBA:

    u16 type; u16 status; u32 freq; u32 loopStart; u32 size; s8 data[];

all little-endian. The sample bytes after it are signed 8-bit and need no
swapping, but those five fields are read as integers by MidiKeyToFreq and by
the mixer, and on a big-endian build they come out as nonsense -- a sample
rate of 13,700,096 reads back as 0x00D10C00 byte-reversed, which is a pitch
no note ever asked for.

Swapping once here, at build time, keeps the runtime free of per-access
byte-swapping in the mixer's inner loop, and keeps src/m4a.c reading the
struct as plain C the way it was written.

Idempotence matters because make may re-run this on an existing file: the
script is driven from the .wav, so wav2agb always rewrites the .bin first
and this only ever sees a freshly little-endian header.
"""
import struct
import sys

HEADER = "<HHIII"
HEADER_SIZE = struct.calcsize(HEADER)

def main():
    if len(sys.argv) != 2:
        sys.exit(f"usage: {sys.argv[0]} <sample.bin>")

    path = sys.argv[1]
    with open(path, "rb") as f:
        data = bytearray(f.read())

    if len(data) < HEADER_SIZE:
        sys.exit(f"{path}: too short to hold a WaveData header")

    fields = struct.unpack_from(HEADER, data, 0)
    struct.pack_into(">HHIII", data, 0, *fields)

    with open(path, "wb") as f:
        f.write(data)


if __name__ == "__main__":
    main()
