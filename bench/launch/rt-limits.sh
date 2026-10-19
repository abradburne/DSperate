#!/bin/bash
# On the handheld, as root, with no game running: starts each ROM the way DSlot does
# (`dslot --launch ROM`: DSperate's description, settings, saves folder and environment) under
# the limits DSlot's games really run with (dslot.service: its User, LimitRTPRIO and Nice, and
# PipeWire's client library adding RLIMIT_RTTIME of 200 ms), leaves it SECONDS to run, then asks
# it to quit. A game the kernel killed for running real-time too long without a sleep (signal 9,
# typically while a big zipped ROM inflates or a state loads) fails the run. DSlot is stopped for
# the run, so the game has the panels, and started again after, whatever happens.
#
#   sudo bench/launch/rt-limits.sh [-s SECONDS] [-e DSPERATE] ROM...
#
# -e runs that dsperate binary directly instead, with the arguments DSlot would give it.
# With no ROMs: the default set below (ROMS, default /mnt/ds/Roms), the biggest zipped GBA games
# (8 and 16 MB inside) and a 512 MB DS game.
set -u
secs=6 emu=
while getopts s:e: o; do
  case $o in s) secs=$OPTARG ;; e) emu=$OPTARG ;; *) exit 2 ;; esac
done
shift $((OPTIND - 1))
roms=${ROMS:-/mnt/ds/Roms}
if [ $# -eq 0 ]; then
  set -- "$roms/GBA/Advance Wars 2 - Black Hole Rising (USA).zip" \
         "$roms/GBA/Donkey Kong Country 2 (USA).zip" \
         "$roms/GBA/DK - King of Swing (Europe) (En,Fr,De,Es,It).zip" \
         "$roms/NDS/Pokemon - Black Version 2 (USA, Europe) (NDSi Enhanced).nds"
fi
[ "$(id -u)" = 0 ] || { echo "rt-limits: run as root (systemd-run, DSlot)" >&2; exit 2; }
if pgrep -x dsperate > /dev/null; then echo "rt-limits: a game is running; try later" >&2; exit 2; fi
dslot=$(command -v dslot || echo /usr/bin/dslot)
[ -n "$emu" ] || [ -x "$dslot" ] || { echo "rt-limits: no $dslot (DSlot 0.3 or later launches the games)" >&2; exit 2; }
user=$(systemctl show -P User dslot.service)
uid=$(id -u "$user")
home=$(getent passwd "$user" | cut -d: -f6)
props=(-p User="$user" -p LimitRTPRIO="$(systemctl show -P LimitRTPRIO dslot.service)"
       -p Nice="$(systemctl show -P Nice dslot.service)" -p RuntimeMaxSec=$((secs + 30)))
dslot_was=$(systemctl is-active dslot.service)
[ "$dslot_was" = active ] && systemctl stop dslot.service
trap '[ "$dslot_was" = active ] && systemctl start dslot.service' EXIT

fail=0 n=0
for rom in "$@"; do
  n=$((n + 1))
  [ -r "$rom" ] || { echo "MISSING $rom"; fail=1; continue; }
  unit=rt-limits-$$-$n
  if [ -n "$emu" ]; then
    # What DSlot would run: GBA by its folder or extension, saves beside DSlot's.
    name=${rom##*/}; name=${name%.*}
    case "$rom" in */GBA/*|*.[gG][bB][aA]) sys=GBA mode=--gba ;; *) sys=NDS mode= ;; esac
    saves=/mnt/roms/Saves/$sys/$name
    install -d -o "$user" "$saves" 2>/dev/null
    cmd=("$emu" ${mode:+"$mode"} --saves "$saves" --states "$saves" "$rom")
    env=(-E DS_DEBUG=marks -E SDL_VIDEODRIVER=kmsdrm -E SDL_KMSDRM_ATOMIC=0)
  else
    cmd=("$dslot" --launch "$rom")
    env=(-E SDL_VIDEODRIVER=kmsdrm)
  fi
  systemd-run --unit="$unit" --collect --quiet "${props[@]}" \
    -E XDG_RUNTIME_DIR="/run/user/$uid" -E HOME="$home" "${env[@]}" "${cmd[@]}"
  sleep "$secs"
  alive=$(systemctl is-active "$unit" 2>/dev/null)
  pid=$(pgrep -x dsperate | head -1)
  rttime=$([ -n "$pid" ] && awk '/Max realtime timeout/ {print $4}' "/proc/$pid/limits")
  systemctl kill --signal=TERM "$unit" 2>/dev/null
  for _ in $(seq 100); do systemctl is-active -q "$unit" || break; sleep 0.1; done
  systemctl kill --signal=KILL "$unit" 2>/dev/null
  if [ "$alive" = active ]; then
    echo "OK      ${rom##*/} (ran ${secs} s; RLIMIT_RTTIME ${rttime:-?} us)"
  else
    echo "FAILED  ${rom##*/}: gone before ${secs} s"
    journalctl -u "$unit" -o cat --no-pager | grep -E "signal|status=|KILL|error|cannot|could not" | head -5 | sed 's/^/        /'
    fail=1
  fi
done
exit $fail
