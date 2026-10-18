#!/bin/bash
# On the handheld, as root, with no game running: starts each ROM through DSlot's launcher under
# the limits DSlot's games really run with (dslot.service: User, LimitRTPRIO=10, PipeWire's client
# library adding RLIMIT_RTTIME of 200 ms), leaves it SECONDS to run, then asks it to quit. A game
# the kernel killed for running real-time too long without a sleep (signal 9, typically while a big
# zipped ROM inflates or a state loads) fails the run. DSlot is stopped for the run, so the game
# has the panels, and started again after.
#
#   sudo bench/launch/rt-limits.sh [-s SECONDS] [-e DSPERATE] ROM...
#
# With no ROMs: the default set below (ROMS, default /mnt/ds/Roms), the biggest zipped GBA games
# (8 and 16 MB inside) and a 512 MB DS game. -e runs another dsperate binary than the package's.
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
user=$(systemctl show -P User dslot.service)
rtprio=$(systemctl show -P LimitRTPRIO dslot.service)
uid=$(id -u "$user")
launch=${LAUNCH:-/usr/lib/dslot/launch}   # LAUNCH: another launcher for a test run
dslot_was=$(systemctl is-active dslot.service)
[ "$dslot_was" = active ] && systemctl stop dslot.service
trap '[ "$dslot_was" = active ] && systemctl start dslot.service' EXIT

fail=0 n=0
for rom in "$@"; do
  n=$((n + 1))
  [ -r "$rom" ] || { echo "MISSING $rom"; fail=1; continue; }
  unit=rt-limits-$$-$n
  out=$(mktemp)
  systemd-run --unit="$unit" --wait --collect --quiet -p User="$user" -p LimitRTPRIO="$rtprio" \
    -E XDG_RUNTIME_DIR="/run/user/$uid" -E HOME="$(getent passwd "$user" | cut -d: -f6)" \
    ${emu:+-E DSPERATE="$emu"} "$launch" "$rom" > "$out" 2>&1 &
  waiter=$!
  sleep "$secs"
  alive=$(systemctl is-active "$unit" 2>/dev/null)
  rttime=$(pid=$(systemctl show -P MainPID "$unit"); [ "$pid" -gt 0 ] 2>/dev/null && awk '/Max realtime timeout/ {print $4}' "/proc/$pid/limits")
  systemctl kill --signal=TERM "$unit" 2>/dev/null
  wait "$waiter"
  log=$(journalctl -u "$unit" -o cat --no-pager 2>/dev/null; cat "$out")
  rm -f "$out"
  if [ "$alive" = active ]; then
    echo "OK      ${rom##*/} (ran ${secs} s; RLIMIT_RTTIME ${rttime:-?} us)"
  else
    echo "FAILED  ${rom##*/}: gone before ${secs} s"
    echo "$log" | grep -E "signal|status=|KILL|error|cannot|could not" | head -5 | sed 's/^/        /'
    fail=1
  fi
done
exit $fail
