#!/bin/bash
# tools/emu_mupen.sh <rom> <outdir> — boot a ROM under mupen64plus with the
# angrylion RDP/VI plugin on a private Xvfb and screenshot it.
set -u
ROM="$(readlink -f "$1")"; OUT="$2"; SETTLE="${SETTLE:-100}"
rm -rf "$OUT"; mkdir -p "$OUT"
pkill -9 -x mupen64plus 2>/dev/null; pkill -9 -x Xvfb 2>/dev/null; sleep 2
rm -f /tmp/.X99-lock /tmp/.X11-unix/X99
Xvfb :99 -screen 0 800x600x24 -nolisten tcp > "$OUT/xvfb.log" 2>&1 &
XPID=$!; export DISPLAY=:99; sleep 6
setsid "${MUPEN_BIN:-/usr/games/mupen64plus}" --emumode 2 --gfx mupen64plus-video-angrylion-plus \
    --audio dummy --input dummy --rsp mupen64plus-rsp-hle \
    --windowed --resolution 640x480 "$ROM" > "$OUT/emu.log" 2>&1 < /dev/null &
sleep "$SETTLE"
WID=$(timeout 10 xdotool search --class -- mupen64plus 2>/dev/null | head -1)
[ -n "$WID" ] && timeout 20 import -window "$WID" "$OUT/shot.png" 2>/dev/null
[ -s "$OUT/shot.png" ] || timeout 20 import -window root "$OUT/shot.png" 2>/dev/null
pkill -9 -x mupen64plus 2>/dev/null; sleep 1; kill -9 "$XPID" 2>/dev/null
