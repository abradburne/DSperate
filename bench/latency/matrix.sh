#!/bin/bash
# On the DS+: latency.sh for each game and setting set (DSlot stopped), into OUT/<game>-<set>/.
#   bench/latency/matrix.sh OUT RUNS [sets...]     sets: configs/<set>.ini (default: base input lock lock-input lock-vblank)
# Games: NSMB on the world map (right), Golden Sun: Dark Dawn at 1x in a dialogue (A),
# Advance Wars at its title (Start). States in /tmp/lat-states: nsmb.dss, gs.dss (auto slots),
# aw2400.dss (dsperate-headless --gba --frames 2400 --save-state-at 2400:FILE).
out=$1 runs=$2; shift 2
sets=("$@"); [ $# -eq 0 ] && sets=(base input lock lock-input lock-vblank)
here=$(cd "$(dirname "$0")" && pwd)
nds=${ROMS:-/mnt/ds/Roms}/NDS gba=${ROMS:-/mnt/ds/Roms}/GBA st=${STATES:-/tmp/lat-states}
for s in "${sets[@]}"; do
  LAT_INI="$here/configs/$s.ini" "$here/latency.sh" "$nds/New Super Mario Bros. (USA).nds" "$st/nsmb.dss" right "$runs" "$out/nsmb-$s"
  LAT_INI="$here/configs/$s.ini $here/configs/1x.ini" "$here/latency.sh" "$nds/Golden Sun - Dark Dawn (USA, Australia) (En,Es).nds" "$st/gs.dss" a "$runs" "$out/gs1x-$s"
  LAT_INI="$here/configs/$s.ini" "$here/latency.sh" "$gba/Advance Wars (USA).zip" "$st/aw2400.dss" start "$runs" "$out/aw-$s" --gba
done
echo MATRIX DONE
