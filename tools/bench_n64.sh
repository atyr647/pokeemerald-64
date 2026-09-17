#!/bin/bash
# tools/bench_n64.sh <label>
#
# Run the profiling benchmark on both emulators and print the comparison.
#
# Why both:
#   mupen64plus + angrylion  fast to iterate, and its clock is close to one
#                            CP0 count per instruction -- but it models no
#                            caches and no memory latency, so it measures
#                            instruction count and nothing else.
#   ares                     models the VR4300's caches and bus timing, so
#                            its numbers include the stalls that dominate a
#                            93.75 MHz CPU with an 8 KB data cache. Slower to
#                            run; where the two disagree, this is the one to
#                            believe.
#
# The ROM sums a fixed span of VBlanks and freezes the totals (see
# N64_PROFILE_OVERLAY in src/n64/gpu_regs_n64.c), so both emulators measure
# the same stretch of the same scene and the run is reproducible to the
# count. The headline number is `frames`: how many frames were composited
# inside that span of game time.
#
# Requires N64_PROFILE_OVERLAY set to 1 and both ROMs built.
set -u
LABEL="${1:-run}"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="${BENCH_OUT:-/tmp/bench_n64}"
LABELS="bg,spr,unused,idle,frames,regs,spike_frames,spike_scanline,spike_affine,spike_window,spike_bpp8,calib300k"

mkdir -p "$OUT"

"$ROOT/tools/emu_mupen.sh" "$ROOT/build/n64/pokeemerald64-emu.z64" "$OUT/mupen" >/dev/null 2>&1
"$ROOT/tools/emu_ares.sh"  "$ROOT/build/n64/pokeemerald64.z64"     "$OUT/ares"  >/dev/null 2>&1

echo "===== $LABEL"
read_one() {
    python3 "$ROOT/tools/decode_profile.py" --labels "$LABELS" "$1" 2>/dev/null | awk '
        /^  bg /{bg=$2} /^  spr /{spr=$2} /^  idle /{id=$2} /^  frames /{f=$2} /^  calib300k /{cal=$2}
        END { if (f == "" || bg == "") print "decode failed";
              else printf "frames=%-6s bg=%-13s spr=%-12s idle=%-12s calib=%s\n", f, bg, spr, id, cal }'
}
printf "%-22s " "mupen64plus+angrylion"; read_one "$OUT/mupen/shot.png"
printf "%-22s " "ares"                 ; read_one "$OUT/ares/shot.png"
