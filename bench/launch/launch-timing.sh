#!/bin/bash
# On the handheld, with dslot.service running and the shelf up: presses A on the shelf, waits, presses
# Menu in the game, waits for the shelf, N times over, and prints the marks DSlot and DSperate
# leave in the journal (CLOCK_BOOTTIME), with the press times from press.py.
#
#   sudo bench/launch/launch-timing.sh [rounds] [seconds in the game]
rounds=${1:-3}
play=${2:-8}
here=$(cd "$(dirname "$0")" && pwd)
pad="Anbernic RG DS+ Gamepad"
since=$(date +%s)
for _ in $(seq "$rounds"); do
  "$here/press.py" "$pad" BTN_EAST
  sleep "$play"
  "$here/press.py" "$pad" BTN_MODE
  sleep 5
done
journalctl -u dslot --since "@$since" -o cat --no-pager | grep -E "^(dslot|dsperate): [0-9]" |
  grep -E "insert started|game starting|started|SDL ready|first frame presented|leaving|saved|closed|game exited|first frame shown"
