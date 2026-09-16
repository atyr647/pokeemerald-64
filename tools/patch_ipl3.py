#!/usr/bin/env python3
"""
tools/patch_ipl3.py — Assemble ipl3.s and patch it into an N64 ROM file.

Usage:
  python3 tools/patch_ipl3.py --bin <ipl3.bin> <rom.z64>
      Uses a pre-built IPL3 binary (e.g. extracted from elite_newkind.z64
      via tools/extract_ipl3.py) instead of assembling ipl3.s.
      This is what the hardware ROM uses — libdragon's IPL3 brings RDRAM
      up itself and is recognized by SC64.

  python3 tools/patch_ipl3.py [--elf <rom.elf>] <rom.z64>
      Assembles tools/ipl3.s and patches it into the ROM.  This is the
      emulator ROM's boot stub (see `make -f Makefile.n64 emu`); it skips
      RDRAM init, which is what mupen64plus and its derivatives need.
      The stub is laid out from the linked ELF's .boot section, so the ELF
      has to be findable: it defaults to the ROM's sibling .elf, which is
      where the build leaves it, and --elf points elsewhere.

  python3 tools/patch_ipl3.py ... --emit-json <out.json>
      Also writes the padded header region as base64 in a small JSON file.
      web/index.html loads that to offer the emulator ROM next to the
      hardware one, since it cannot assemble the stub itself.  Because the
      blob is laid out from a specific ELF, regenerate it whenever the ROM
      is rebuilt, exactly like the patch.

Steps:
  1. Get IPL3 bytes: either assemble ipl3.s or read --bin file
  2. Verify the binary fits in 4032 bytes (0x040–0x0FFF)
  3. Patch the binary into the ROM at offset 0x040
"""

import base64
import json
import os
import subprocess
import sys
import tempfile

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
REPO_ROOT   = os.path.dirname(SCRIPT_DIR)
IPL3_SRC    = os.path.join(SCRIPT_DIR, "ipl3.s")
IPL3_SIZE   = 0xFC0          # 4032 bytes — bytes 0x040–0x0FFF of ROM header
IPL3_OFFSET = 0x040          # byte offset in ROM where IPL3 starts

def boot_section_layout(elf_path):
    """Where .boot lives, read from the linked ELF.

    The stub has to copy .boot out of the cartridge and jump to it, so it
    needs that section's cartridge address, its link address and its size.
    Reading them here rather than hardcoding them in the assembly is what
    keeps the stub from going stale when n64.ld moves things around.
    """
    out = subprocess.run(
        ["mipsel-linux-gnu-readelf", "-S", "-W", elf_path],
        capture_output=True, text=True)
    if out.returncode:
        print("readelf error:", out.stderr)
        sys.exit(1)

    vma = size = None
    for line in out.stdout.splitlines():
        parts = line.replace("[", " ").replace("]", " ").split()
        # e.g.  2 .boot PROGBITS 80360000 020000 000640 00 AX 0 0 32
        if len(parts) >= 7 and parts[1] == ".boot" and parts[2] == "PROGBITS":
            vma = int(parts[3], 16)
            size = int(parts[5], 16)
            break
    if vma is None:
        print(f"error: no .boot section in {elf_path}")
        sys.exit(1)

    # The cartridge address is the section's LMA, which readelf -S does not
    # print; take it from the program header that loads this VMA.
    out = subprocess.run(
        ["mipsel-linux-gnu-readelf", "-l", "-W", elf_path],
        capture_output=True, text=True)
    lma = None
    for line in out.stdout.splitlines():
        parts = line.split()
        if len(parts) >= 5 and parts[0] == "LOAD":
            try:
                if int(parts[2], 16) == vma:
                    lma = int(parts[3], 16)
                    break
            except ValueError:
                continue
    if lma is None:
        print(f"error: no LOAD segment for .boot at {vma:#x}")
        sys.exit(1)

    # The cache loop steps 32 bytes at a time and exits on equality, so the
    # size has to be a whole number of cache lines or it runs away.
    size = (size + 31) & ~31
    return lma, vma, size


def assemble_ipl3(elf_path):
    """Assemble ipl3.s against a given ELF's layout, return raw bytes."""
    lma, vma, size = boot_section_layout(elf_path)
    print(f"  .boot: cart {lma:#010x} -> {vma:#010x}, {size} bytes")

    with tempfile.TemporaryDirectory() as tmpdir:
        obj  = os.path.join(tmpdir, "ipl3.o")
        raw  = os.path.join(tmpdir, "ipl3.bin")

        # Assemble: big-endian MIPS III, no ABI calls
        as_cmd = [
            "mipsel-linux-gnu-as",
            "-EB", "-mips3", "-mabi=32", "-G0",
            f"--defsym=IPL3_BOOT_LMA={lma}",
            f"--defsym=IPL3_BOOT_VMA={vma}",
            f"--defsym=IPL3_BOOT_SIZE={size}",
            IPL3_SRC, "-o", obj
        ]
        r = subprocess.run(as_cmd, capture_output=True, text=True)
        if r.returncode:
            print("Assembler error:", r.stderr)
            sys.exit(1)

        # Extract raw binary
        oc_cmd = [
            "mipsel-linux-gnu-objcopy",
            "-O", "binary",
            "--only-section=.text",
            obj, raw
        ]
        r = subprocess.run(oc_cmd, capture_output=True, text=True)
        if r.returncode:
            print("objcopy error:", r.stderr)
            sys.exit(1)

        with open(raw, "rb") as f:
            data = f.read()
    return data

def pad_ipl3(ipl3_bytes):
    if len(ipl3_bytes) > IPL3_SIZE:
        print(f"IPL3 too large: {len(ipl3_bytes)} > {IPL3_SIZE}")
        sys.exit(1)
    # Pad to exactly IPL3_SIZE with NOPs (0x00000000 = NOP in BE MIPS)
    return ipl3_bytes + b'\x00' * (IPL3_SIZE - len(ipl3_bytes))


def emit_json(json_path, ipl3_bytes):
    """Write the padded header region for web/index.html to splice in."""
    padded = pad_ipl3(ipl3_bytes)
    with open(json_path, "w") as f:
        json.dump({
            "format": "ipl3+base64",
            "offset": IPL3_OFFSET,
            "length": IPL3_SIZE,
            "data": base64.b64encode(padded).decode("ascii"),
        }, f)
    print(f"wrote {json_path} ({IPL3_SIZE} bytes at {IPL3_OFFSET:#x})")


def patch_rom(rom_path, ipl3_bytes):
    padded = pad_ipl3(ipl3_bytes)

    with open(rom_path, "r+b") as f:
        f.seek(IPL3_OFFSET)
        f.write(padded)

    print(f"IPL3 patched into {rom_path}")
    print(f"  Code size: {len(ipl3_bytes)} bytes  Padded to: {IPL3_SIZE} bytes")

def main():
    args = sys.argv[1:]

    # Parse --bin <file> flag
    bin_path = None
    if "--bin" in args:
        idx = args.index("--bin")
        if idx + 1 >= len(args):
            print("Error: --bin requires a file argument")
            sys.exit(1)
        bin_path = args[idx + 1]
        args = args[:idx] + args[idx + 2:]

    # --elf <file>: layout source for the assembled stub (defaults to the
    # ROM's sibling .elf, which is where the build leaves it).
    elf_path = None
    if "--elf" in args:
        idx = args.index("--elf")
        if idx + 1 >= len(args):
            print("Error: --elf requires a file argument")
            sys.exit(1)
        elf_path = args[idx + 1]
        args = args[:idx] + args[idx + 2:]

    # --emit-json <file>: also drop the padded blob next to the web patch.
    json_path = None
    if "--emit-json" in args:
        idx = args.index("--emit-json")
        if idx + 1 >= len(args):
            print("Error: --emit-json requires a file argument")
            sys.exit(1)
        json_path = args[idx + 1]
        args = args[:idx] + args[idx + 2:]

    if not args:
        print(f"Usage: {sys.argv[0]} [--bin <ipl3.bin>] [--elf <rom.elf>] "
              f"[--emit-json <out.json>] <rom.z64>")
        sys.exit(1)

    rom_path = args[0]
    if not os.path.exists(rom_path):
        print(f"ROM not found: {rom_path}")
        sys.exit(1)

    if bin_path:
        if not os.path.exists(bin_path):
            print(f"IPL3 binary not found: {bin_path}")
            sys.exit(1)
        with open(bin_path, "rb") as f:
            ipl3_bytes = f.read()
        print(f"Using pre-built IPL3: {bin_path} ({len(ipl3_bytes)} bytes)")
    else:
        if elf_path is None:
            elf_path = os.path.splitext(rom_path)[0] + ".elf"
        if not os.path.exists(elf_path):
            print(f"error: need the linked ELF to lay out the stub; {elf_path} not found")
            print("       pass --elf <file> if it lives elsewhere")
            sys.exit(1)
        print("Assembling IPL3...")
        ipl3_bytes = assemble_ipl3(elf_path)
        print(f"IPL3 assembled: {len(ipl3_bytes)} bytes")

    # Verify first instruction looks sane (should be a LUI or similar)
    if len(ipl3_bytes) >= 4:
        first_word = int.from_bytes(ipl3_bytes[:4], 'big')
        print(f"  First instruction: 0x{first_word:08X}")
        if first_word == 0x3044d236:
            print("  (libdragon IPL3 — recognized by SC64)")

    patch_rom(rom_path, ipl3_bytes)

    if json_path:
        emit_json(json_path, ipl3_bytes)

if __name__ == "__main__":
    main()
