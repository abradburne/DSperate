#!/bin/bash
# On the handheld: whether two DSperate builds put exactly the same pictures on the panels. Each runs
# the game from boot for FRAMES frames with the installed settings plus the overrides given,
# every frame shown (DS_DEBUG=present-wait, frame-skip off), and writes a hash of each window's
# picture per present (present-hash; the GLES present, so hires3d >= 2 or the GLES 1x path).
# Builds from before DS_DEBUG (DSperate fork, 2026-10-18) read DS_PRESENT_WAIT / DS_PRESENT_HASH,
# so both are set: either build can be BIN_A or BIN_B.
# Prints the first differing presents, or "same". Both runs replay one input log (no input,
# recorded once per ROM and frame count in ~/render-logs): under --replay the RTC is frozen,
# so games that read the date (New Super Mario Bros. from frame ~889) run the same each time.
#
# STATE=<file.dss> starts both from that save state, without a replay (a replay counts frames from
# boot, so it cannot follow a state): check that a build matches itself there first.
#
#   [HIRES=2] [STATE=f.dss] bench/render/render-same.sh BIN_A BIN_B ROM FRAMES [ini lines...]   (DSlot stopped)
# shellcheck disable=SC2034 # b is read as ${!k} below
a=$1 b=$2 rom=$3 frames=$4
shift 4
scratch=/tmp/render-same/${OUT:-run}
rm -rf "$scratch" && mkdir -p "$scratch/saves"
cp ~/.config/dsperate/dsperate.ini "$scratch/dsperate.ini"
{ echo "[video]"; echo "hires3d = ${HIRES:-2}"; echo "hires3d_fallback = false"; echo "[emu]"; echo "frameskip = 0"; for l in "$@"; do echo "$l"; done; } >> "$scratch/dsperate.ini"
log=~/render-logs/$(basename "$rom" | tr -c 'A-Za-z0-9.\n' _)${STATE:+.$(basename "$STATE")}.$frames.dsin
if [ -z "$STATE" ] && [ ! -f "$log" ]; then
  mkdir -p ~/render-logs
  env SDL_VIDEODRIVER=kmsdrm SDL_KMSDRM_ATOMIC=0 XDG_RUNTIME_DIR="/run/user/$(id -u)" \
    "$a" --config "$scratch/dsperate.ini" --no-autoload --saves "$scratch/saves" --states "$scratch/saves" \
    --frames "$frames" --record "$log" "$rom" > "$scratch/record.log" 2>&1
fi
src=(--replay "$log"); [ -n "$STATE" ] && src=(--load-state "$STATE")
for k in a b; do
  bin=${!k}
  rm -rf "$scratch/saves"/*
  env SDL_VIDEODRIVER=kmsdrm SDL_KMSDRM_ATOMIC=0 XDG_RUNTIME_DIR="/run/user/$(id -u)" \
    DS_DEBUG="present-wait,present-hash=$scratch/$k.txt" DS_PRESENT_WAIT=1 DS_PRESENT_HASH="$scratch/$k.txt" \
    "$bin" --config "$scratch/dsperate.ini" --no-autoload --saves "$scratch/saves" --states "$scratch/saves" \
    --frames "$frames" "${src[@]}" "$rom" > "$scratch/$k.log" 2>&1
done
# The first three fields: window, present, picture hash (a later build adds what a 1x window should show).
python3 - "$scratch/a.txt" "$scratch/b.txt" <<'PY'
import sys
a, b = ([l.split()[:3] for l in open(f)] for f in sys.argv[1:3])
d = [i for i in range(min(len(a), len(b))) if a[i] != b[i]]
print(f"presents: {len(a)} / {len(b)}; " + ("same" if not d and len(a) == len(b) else f"{len(d)} differ, first: " + ", ".join(" ".join(a[i]) for i in d[:4])))
PY
