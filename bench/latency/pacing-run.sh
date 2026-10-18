#!/bin/bash
# On the DS+: one game for SECS seconds with no input, logged with DS_LATENCY, for pacing.py
# (repeated and dropped frames per panel). Installed settings (frame-skip included) plus LAT_INI.
#
#   bench/latency/pacing-run.sh ROM STATE SECS OUT.log [dsperate options...]
rom=$1 state=$2 secs=$3 log=$4
shift 4
scratch=/tmp/dsperate-pacing
rm -rf "$scratch" && mkdir -p "$scratch/cfg/dsperate" "$scratch/saves"
sed -e '/^\[emu\]/,/^\[/{/^autosave\|^autoload/d}' ~/.config/dsperate/dsperate.ini > "$scratch/cfg/dsperate/dsperate.ini"
cat >> "$scratch/cfg/dsperate/dsperate.ini" <<EOF
[paths]
saves = $scratch/saves
states = $scratch/saves
EOF
# shellcheck disable=SC2086 # LAT_INI is a list of files
[ -n "$LAT_INI" ] && cat $LAT_INI >> "$scratch/cfg/dsperate/dsperate.ini"
sudo prlimit --pid $$ --rtprio=10:10
timeout -s TERM "$secs" env SDL_VIDEODRIVER=kmsdrm SDL_KMSDRM_ATOMIC=0 XDG_RUNTIME_DIR="/run/user/$(id -u)" \
  XDG_CONFIG_HOME="$scratch/cfg" DS_DEBUG="latency=$log" DS_LATENCY="$log" DS_VERBOSE=1 DS_FPS=1 \
  "${DSP:-dsperate}" --no-autoload --load-state "$state" "$@" "$rom" > "${log%.log}.err" 2>&1
echo "done: $log"
