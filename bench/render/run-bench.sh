#!/bin/bash
# On the handheld: one DSperate build, each game 65 s from boot with no input, on both panels in
# real time, frame-skip off and no sound. The first 300 frames (5 s) are left out of the
# statistics. For the packaged build with sound through PipeWire, see bench-realtime.sh.
#
#   bench/render/run-bench.sh [dsperate binary]   (DSlot not running)  -> /tmp/emu-bench.txt
#
# ROMS (default /mnt/ds/Roms) holds the games, in NDS/.
emu=${1:-/usr/bin/dsperate}
out=/tmp/emu-bench.txt
home=/tmp/dsperate-home
mkdir -p "$home"
: > "$out"
games=(
  "Castlevania - Dawn of Sorrow (USA).nds"
  "New Super Mario Bros. (USA).nds"
  "Pokemon - Platinum Version (USA) (Rev 1).nds"
  "Mario Kart DS (USA) (En,Fr,De,Es,It).nds"
  "Super Mario 64 DS (USA) (Rev 1).nds"
  "Legend of Zelda, The - Spirit Tracks (USA) (En,Fr,Es) (Rev 1).nds"
  "Golden Sun - Dark Dawn (USA, Australia) (En,Es).nds"
)
for g in "${games[@]}"; do
  rom="${ROMS:-/mnt/ds/Roms}/NDS/$g"
  echo "=== $g" >> "$out"
  timeout -s TERM 66 env SDL_VIDEODRIVER=kmsdrm SDL_AUDIODRIVER=dummy DS_FRAME_STATS=1 HOME="$home" \
    "$emu" --dual-window --fullscreen --frameskip 0 --stats-from 300 "$rom" > /tmp/dsp-one.log 2>&1
  grep -a -E "^frame ms|^frame budget|^work budget" /tmp/dsp-one.log >> "$out"
  sleep 2
done
echo DONE >> "$out"
