#!/bin/bash
# On the DS+: button-to-screen runs for one game. Each run starts DSperate from the same save
# state with DS_LATENCY logging, presses KEY once at a random moment with lat-press, and quits.
# The first two runs press nothing: the baseline the presses are compared against (the
# emulation is deterministic from a state, so the first frame that differs from the baseline is
# the game's response). analyse.py turns the folder into numbers.
#
#   bench/latency/latency.sh ROM STATE KEY RUNS OUT [dsperate options...]
#
# As the handheld's user, the launcher stopped (tools/device-run.sh), lat-press in /usr/local/bin.
# DS_LATENCY is set too, for builds from before DS_DEBUG.
# Real-time priority as dslot.service gives it (LimitRTPRIO=10). Saves go to a scratch folder.
# DSP: the dsperate binary (default: the installed one). --frameskip 0 keeps runs comparable.
rom=$1 state=$2 key=$3 runs=$4 out=$5
shift 5
scratch=/tmp/dsperate-latency
rm -rf "$scratch" "$out" && mkdir -p "$scratch/cfg/dsperate" "$scratch/saves" "$out"
# The installed settings, with saves kept away from the game's own and no auto slot.
sed -e '/^\[emu\]/,/^\[/{/^autosave\|^autoload/d}' ~/.config/dsperate/dsperate.ini > "$scratch/cfg/dsperate/dsperate.ini"
cat >> "$scratch/cfg/dsperate/dsperate.ini" <<EOF
[paths]
saves = $scratch/saves
states = $scratch/saves
EOF
# LAT_INI: files of more settings for this set (space-separated), appended: later keys win.
# shellcheck disable=SC2086 # LAT_INI is a list of files
[ -n "$LAT_INI" ] && cat $LAT_INI >> "$scratch/cfg/dsperate/dsperate.ini"
cp "$scratch/cfg/dsperate/dsperate.ini" "$out/settings.ini"
sudo prlimit --pid $$ --rtprio=10:10
for ((r = 0; r < runs + 2; r++)); do
  log=$out/run$r.log
  env SDL_VIDEODRIVER=kmsdrm SDL_KMSDRM_ATOMIC=0 XDG_RUNTIME_DIR="/run/user/$(id -u)" XDG_CONFIG_HOME="$scratch/cfg" \
    DS_LATENCY="$log" DS_DEBUG="latency=$log" DS_VERBOSE=1 ${LAT_ENV:+$LAT_ENV} \
    "${DSP:-dsperate}" --no-autoload --frameskip 0 --load-state "$state" "$@" "$rom" > "$out/run$r.err" 2>&1 &
  pid=$!
  # From the state's first frames, then a random 0.4-1.2 s so presses land at every phase.
  for ((i = 0; i < 100; i++)); do [ -s "$log" ] && [ "$(wc -l < "$log")" -ge 30 ] && break; sleep 0.05; done
  sleep "$(awk -v s=$RANDOM 'BEGIN { srand(s); printf "%.3f", 0.4 + rand() * 0.8 }')"
  # shellcheck disable=SC2024 # the redirect is ours, the press needs root
  if [ "$r" -ge 2 ]; then sudo lat-press "$key" 100 > "$out/run$r.press"; else sleep 0.1; fi
  sleep 1.2
  kill -TERM "$pid"; wait "$pid" 2>/dev/null
  sleep 0.3
done
echo "done: $out"
