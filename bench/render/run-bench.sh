#!/bin/bash
# On the handheld: each game, 65 s from boot with no input, in DSperate and in DraStic,
# both on the panels in real time, frame-skip off. First 300 frames (5 s) left out.
out=/tmp/emu-bench.txt
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
  cd /tmp/dsperate || exit 1
  timeout -s TERM 66 env SDL_VIDEODRIVER=kmsdrm SDL_AUDIODRIVER=dummy DS_FRAME_STATS=1 HOME=/tmp/dsperate-home \
    ./dsperate --dual-window --fullscreen --frameskip 0 --stats-from 300 "$rom" > /tmp/dsp-one.log 2>&1
  { echo "-- DSperate"; grep -a -E "^frame ms|^frame budget|^work budget" /tmp/dsp-one.log; } >> "$out"
  sleep 2
  cd /tmp/drastic_aarch64 || exit 1
  sed -i "s/^fast_forward = .*/fast_forward = 0/" config/drastic.cfg
  timeout -s KILL 80 env SDL_VIDEODRIVER=kmsdrm SDL_AUDIODRIVER=dummy FRAMELOG_SKIP=300 FRAMELOG_FRAMES=3600 LD_PRELOAD=/tmp/framelog.so \
    ./drastic "$rom" 2>&1 >/dev/null | grep -a -E "^frame ms|^frame budget|^realtime" > /tmp/dr-one.log
  { echo "-- DraStic"; cat /tmp/dr-one.log; } >> "$out"
  sleep 2
done
echo DONE >> "$out"
