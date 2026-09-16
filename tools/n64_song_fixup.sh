#!/bin/sh
# tools/n64_song_fixup.sh [--defs] <file.s>
#
# Make mid2agb's output (and the MPlayDef.s it includes) assemble with MIPS gas.
# Three things differ from the ARM assembler it was written for:
#
#   @ comments      ARM gas treats '@' as a comment character; MIPS gas does
#                   not, and chokes on the banner comments mid2agb writes
#                   above every track. Stripped here.
#
#   .end            mid2agb ends each file with '.end', meaning end of input.
#                   To MIPS gas that is the end of a function body and it
#                   warns about the missing .ent. Dropped.
#
#   .align          GOTO and PATT targets are '.word <label>' sitting inside
#                   the track's command stream at whatever byte offset the
#                   stream reached. ARM gas leaves them there; MIPS gas aligns
#                   data directives to their own size and pads the stream with
#                   zero bytes, so the player reads padding where a command
#                   should be. '.align 0' turns that off for the rest of the
#                   file. mid2agb writes exactly one '.align 2' before the
#                   track data and one before the song header, so putting
#                   '.align 0' after the first packs the streams while the
#                   second still aligns the header, which the C code reads as
#                   a struct and does need aligned.
#
# --defs skips the .align step, for the shared definitions file that holds no
# track data of its own.
set -e

defs=0
if [ "$1" = "--defs" ]; then
    defs=1
    shift
fi

f="$1"
awk -v defs="$defs" '
    { sub(/@.*/, "") }
    $1 == ".end" { next }
    { print }
    !done && defs == 0 && $1 == ".align" && $2 == "2" { print "\t.align\t0"; done = 1 }
' "$f" > "$f.fixed"
mv "$f.fixed" "$f"
