# Emerald64 Cartridge Builder

A single-page, entirely client-side patcher: someone drops in their own
Pokémon Emerald (USA) `.gba` and gets back `pokeemerald64.z64` for an
EverDrive 64 or SC64, plus `pokeemerald64-emu.z64` for an emulator. The ROM
never leaves the browser.

`index.html` is the whole site — no build step, no dependencies, no server.
Host it anywhere static.

## Why a patch and not just the ROM

The N64 build is a recompilation, so its MIPS code and pointer tables have no
counterpart in the GBA ROM and have to travel in the patch. What it *does*
share with a retail cartridge is the bulk asset data — compressed graphics,
tilesets, palettes, map blocks, text — which both builds embed byte-for-byte
identically. The patch reuses those from the user's ROM and carries the rest.

Measured against the current build (`--stats`):

```
where the .z64's bytes come from:
  reused from your GBA ROM      5.60 MB   55.8%
  repeated within the .z64      1.90 MB   18.9%
  carried by the patch          2.54 MB   25.3%
```

So a 10.05 MB ROM ships as a 1.70 MB gzipped patch, and the majority of what
comes out really does come off the user's cartridge.

## Building the patch

The site needs `pokeemerald64.bps` sitting next to `index.html`. Generating it
requires a retail ROM, so it is not checked in — build it once on a machine
that has one:

```sh
make -f Makefile.n64                       # produces build/n64/pokeemerald64.z64
python3 tools/make_bps.py \
        /path/to/pokeemerald.gba \
        build/n64/pokeemerald64.z64 \
        web/pokeemerald64.bps --stats --gzip --json
```

It also wants `emu-ipl3.json` beside it — see *Two ROMs* below:

```sh
make -f Makefile.n64 emu                   # produces …/pokeemerald64-emu.z64
python3 tools/patch_ipl3.py \
        --elf build/n64/pokeemerald64.elf \
        --emit-json web/emu-ipl3.json \
        build/n64/pokeemerald64-emu.z64
```

`--gzip` is worth using: what the patch carries is dominated by MIPS code,
which compresses to roughly 40%. The page sniffs the gzip magic and inflates
transparently.

`--json` writes a base64-in-JSON copy alongside. Some static hosts — the
claude.ai artifact host among them — only serve standard web media types and
will not serve a raw `.bps`. The page tries `pokeemerald64.bps.gz`, then
`pokeemerald64.bps`, then `pokeemerald64.bps.json`, so on an ordinary host the
raw file is used and the JSON copy is just dead weight you can delete.

### No retail ROM on hand?

You don't need one. pokeemerald is a *matching* decompilation: built with
agbcc it reproduces the retail cartridge byte for byte. Build the last
pristine upstream commit (`61674ecd`, the parent of the first N64 commit) in a
worktree and check it against `rom.sha1`:

```sh
git worktree add /tmp/pristine 61674ecd
cp -r tools/agbcc /tmp/pristine/tools/     # from github.com/pret/agbcc
cd /tmp/pristine && make -j"$(nproc)"
sha1sum -c rom.sha1                        # must print OK
```

Use `/tmp/pristine/pokeemerald.gba` as the patch source. Do not use this
repo's own GBA build — see below.

### This repo's GBA target no longer matches

Building `make` at HEAD produces a ROM that is *not* byte-identical to retail
(`591eb873…` rather than `f3ae0881…`). Some of the N64 port's edits to shared
source change GBA codegen even though they are semantically inert there — the
`CopyMapBlocks` helper in `fieldmap.c` and the unconditional `MAP_ASSET_16`
wrapping in `battle_pyramid.c`, `decoration.c`, `secret_base.c` and
`trainer_hill.c` are the known ones. A patch built against that ROM would
reject every real cartridge dump, so always build the source ROM from the
pristine commit until matching is restored.

The source ROM must be Pokémon Emerald (USA),
sha1 `f3ae088181bf583e55daf962a92bb46f4f1d07b7`. `make_bps.py` refuses anything
else unless you pass `--allow-any-source`, and it applies the patch back and
compares against the target before writing, so a patch that exists is a patch
that round-trips.

Regenerate the patch on every `.z64` rebuild — it is pinned to that exact
output by CRC-32.

## Two ROMs

The page hands back the same game twice, differing only in the 4 KB boot
header:

- `pokeemerald64.z64` carries libdragon's IPL3, which initialises RDRAM the
  way a console needs and which SC64 recognises. This is the flash cart ROM.
- `pokeemerald64-emu.z64` carries the stub from `tools/ipl3.s`, which leaves
  RDRAM alone. mupen64plus and the cores built on it emulate the RDRAM
  registers by pattern-matching Nintendo's IPL3, so libdragon's derails them
  before the game starts; doing nothing is the right procedure there, because
  their RDRAM is a plain host buffer that works from power-on.

Everything past `0x1000` is byte-identical, and the N64 checksum only covers
`0x1000` onwards, so the page builds the second ROM by splicing the header
from `emu-ipl3.json` into the first. That blob is laid out against a specific
build's `.boot` section, so regenerate it whenever the `.z64` is rebuilt,
exactly like the patch. If it is missing the page quietly falls back to
handing back the hardware ROM on its own.

## Without a bundled patch

If `pokeemerald64.bps` is missing, the page says so and offers a second drop
target for the `.bps`, so it stays usable for anyone who built their own.

## Saving the result

Both ROMs travel in one `pokeemerald64.zip` (deflate, roughly 44% of the raw
size). Self-hosted that is an ordinary download; published as a claude.ai
artifact, page-initiated downloads are blocked and the host's allowlist takes
`.zip` but not `.z64`, so it goes out through the `downloads` capability
instead. Both paths produce the same bytes. With no `emu-ipl3.json` there is
only one ROM to hand over, and self-hosted it is downloaded as a bare `.z64`.

## Checks the page makes

- SHA-1 of the input against the known-good Emerald (USA) hash
- The GBA header's internal title and game code, so a wrong game is named
  (`AXVE` → "That is Pokémon Ruby") rather than rejected as a hash mismatch
- CRC-32 of the source, from the BPS header, before patching
- CRC-32 of the output after patching — a build that completes is verified
