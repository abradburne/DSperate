#!/bin/bash
# bench/core/bench.sh (on the handheld): DSperate's emulation cost per frame, headless and unpaced,
# on fixed scenes (copies of save states in $STATES, so someone's own play does not move them),
# plus each run's frame hashes and audio hash, so a speed-up can be checked for exactness.
# ROMS (default /mnt/ds/Roms) holds NDS/ and GBA/; the scenes name the games they were made with.
#
#   bench.sh BIN OUT [scene...]     BIN = a dsperate-headless; OUT = results dir
# Held out of PGO training: st plat dos gsgba mmz (not in the default ORDER).
# Prints one line per scene: name median mean p95 p99 over% and the frame/audio hashes' md5.
# Stop the launcher first (tools/device-run.sh): DSlot takes ~28% of a core idle on its shelf.
BIN=$1; OUT=$2; shift 2
S=${STATES:-$HOME/dsperate-bench/states}; N=${ROMS:-/mnt/ds/Roms}/NDS; G=${ROMS:-/mnt/ds/Roms}/GBA
mkdir -p "$OUT"
P=""; for f in $(seq 400 150 3000); do P="$P --press $f:start:4 --press $((f+75)):a:4"; done
declare -A SC=(
  [gs1]="--direct --frames 1800 --stats-from 120 --load-state $S/BO5E.1.dss --save $S/gs.sav $N/Golden Sun - Dark Dawn (USA, Australia) (En,Es).nds"
  [gsboot]="--direct --frames 3900 --stats-from 300 $N/Golden Sun - Dark Dawn (USA, Australia) (En,Es).nds"
  [nsmb0]="--direct --frames 1800 --stats-from 120 --load-state $S/A2DE.0.dss --save $S/nsmb.sav $N/New Super Mario Bros. (USA).nds"
  [mkds]="--direct --frames 3900 --stats-from 300 $N/Mario Kart DS (USA) (En,Fr,De,Es,It).nds"
  [ac]="--direct --frames 1800 --stats-from 120 --load-state $S/YAHE.auto.dss --save $S/ac.sav $N/Assassin's Creed - Altair's Chronicles (USA) (En,Fr,Es).nds"
  [aos]="--gba --frames 3600 --stats-from 300 $P $G/Castlevania - Aria of Sorrow (USA).zip"
  [st]="--direct --frames 2400 --stats-from 300 $N/Legend of Zelda, The - Spirit Tracks (USA) (En,Fr,Es) (Rev 1).nds"
  [plat]="--direct --frames 2400 --stats-from 300 $N/Pokemon - Platinum Version (USA) (Rev 1).nds"
  [dos]="--direct --frames 2400 --stats-from 300 $N/Castlevania - Dawn of Sorrow (USA).nds"
  [gsgba]="--gba --frames 2400 --stats-from 300 $P $G/Golden Sun (USA, Europe).zip"
  [mmz]="--gba --frames 2400 --stats-from 300 $P $G/Mega Man Zero (USA, Europe).zip"
  [aw2]="--gba --frames 3600 --stats-from 300 $P $G/Advance Wars 2 - Black Hole Rising (USA).zip"
  [aw2s]="--gba --frames 1800 --stats-from 120 --load-state $S/AW2E.auto.dss $G/Advance Wars 2 - Black Hole Rising (USA).zip"
  [dkc2]="--gba --frames 3600 --stats-from 300 $P $G/Donkey Kong Country 2 (USA).zip"
)
ORDER=(gs1 gsboot nsmb0 mkds ac aos dkc2 aw2 aw2s)
[ $# -gt 0 ] && ORDER=("$@")
for s in "${ORDER[@]}"; do
  a=${SC[$s]}
  rom=${a#*"$N"/}; [ "$rom" = "$a" ] && rom=${a#*"$G"/} && pre=$G || pre=$N
  opts=${a%%"$pre"/*}
  # shellcheck disable=SC2086
  "$BIN" $opts --hash-frames "$OUT/$s.hash" --dump-audio "$OUT/$s.pcm" "$pre/$rom" > "$OUT/$s.log" 2>&1
  m=$(grep -a '^frame ms' "$OUT/$s.log" | awk '{print $4, $6, $10, $12}')
  o=$(grep -a '^frame budget' "$OUT/$s.log" | sed 's/.*(\(.*\)%).*/\1/')
  fh=$(md5sum < "$OUT/$s.hash" | cut -c1-8); ah=$(md5sum < "$OUT/$s.pcm" | cut -c1-8); rm -f "$OUT/$s.pcm"
  printf "%-7s %s over %s%%  frames %s audio %s\n" "$s" "$m" "$o" "$fh" "$ah"
done
