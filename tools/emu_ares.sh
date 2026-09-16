#!/bin/bash
# tools/emu_ares.sh <rom> <outdir> — boot a ROM in ares on a private Xvfb and
# screenshot it.
#
# --kiosk matters: without it the GTK menu bar is a wider white run than the
# overlay's marker, and the decoder locks onto the menu instead.
#
# ares needs a Vulkan device for its RDP; on a machine without one its hidden
# RDRAM pointer stays null and the first RDRAM write segfaults. Mesa's
# software Vulkan (mesa-vulkan-drivers, lavapipe) is enough.
set -u
ROM="$(readlink -f "$1")"; OUT="$2"; SETTLE="${ARES_SETTLE:-120}"
ARES="${ARES_BIN:-/opt/ares/build/desktop-ui/ares}"
rm -rf "$OUT"; mkdir -p "$OUT"
pkill -9 -x ares 2>/dev/null; pkill -9 -x Xvfb 2>/dev/null; sleep 2
rm -f /tmp/.X95-lock /tmp/.X11-unix/X95
Xvfb :95 -screen 0 1024x768x24 -nolisten tcp > "$OUT/xvfb.log" 2>&1 &
XPID=$!; export DISPLAY=:95; sleep 6
setsid "$ARES" --system "Nintendo 64" --no-file-prompt --kiosk \
    --setting Audio/Mute=true "$ROM" > "$OUT/emu.log" 2>&1 < /dev/null &
sleep "$SETTLE"
WID=$(timeout 10 xdotool search --name -- "ares" 2>/dev/null | tail -1)
[ -n "$WID" ] && timeout 20 import -window "$WID" "$OUT/shot.png" 2>/dev/null
[ -s "$OUT/shot.png" ] || timeout 20 import -window root "$OUT/shot.png" 2>/dev/null
pkill -9 -x ares 2>/dev/null; sleep 1; kill -9 "$XPID" 2>/dev/null
