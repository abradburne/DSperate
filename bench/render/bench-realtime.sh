#!/bin/bash
# On the handheld: DSperate as installed (/usr/bin/dsperate), on both panels in real time, sound
# through PipeWire, frame-skip off. Each game 65 s from boot with no input; the first 300 frames
# (5 s) left out of the statistics. Battery saves and states go to a scratch folder, never
# beside the ROMs. The TF2 card must be mounted at /mnt/ds (kernel exFAT). SDL_KMSDRM_ATOMIC=0:
# SDL3's atomic KMSDRM path has no plane for the second panel and crashes tearing it down.
#
#   bench/render/bench-realtime.sh [seconds]      (as the handheld's user, DSlot not running)  -> /tmp/emu-realtime.txt
secs=${1:-65}
out=/tmp/emu-realtime.txt
scratch=/tmp/dsperate-bench
rm -rf "$scratch" && mkdir -p "$scratch/cfg/dsperate" "$scratch/saves"
cat > "$scratch/cfg/dsperate/dsperate.ini" <<EOF
[paths]
saves = $scratch/saves
states = $scratch/saves
EOF
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
  echo "=== $g" >> "$out"
  timeout -s TERM $((secs + 1)) env SDL_VIDEODRIVER=kmsdrm SDL_KMSDRM_ATOMIC=0 XDG_RUNTIME_DIR="/run/user/$(id -u)" \
    XDG_CONFIG_HOME="$scratch/cfg" DS_FRAME_STATS=1 DS_FPS=1 \
    dsperate --dual-window --fullscreen --frameskip 0 --stats-from 300 "${ROMS:-/mnt/ds/Roms}/NDS/$g" > /tmp/dsp-one.log 2>&1
  grep -a -E "^frame ms|^frame budget|^work budget|^fps" /tmp/dsp-one.log | tail -n 8 >> "$out"
  sleep 2
done
echo DONE >> "$out"
