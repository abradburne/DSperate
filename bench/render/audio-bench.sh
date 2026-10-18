#!/bin/bash
# On the handheld: sound break-ups in a heavy game, against settings. Each run is the game from boot
# (no auto slot), or from STATE for N seconds on both panels with the installed settings plus the overrides
# given, real-time priority as under dslot.service when allowed; prints the frames the audio
# queue was found empty ("dry": a gap you hear), how far below budget the frames ran, and fps.
#
#   [STATE=file.dss] [DSPERATE=binary] bench/render/audio-bench.sh ROM SECONDS [ini lines...]       (DSlot stopped)
#   bench/render/audio-bench.sh "/mnt/ds/Roms/NDS/Golden Sun - Dark Dawn (USA, Australia) (En,Es).nds" 65 "[emu]" "frameskip = 1"
rom=$1 secs=$2
shift 2
scratch=/tmp/audio-bench
rm -rf "$scratch" && mkdir -p "$scratch/saves"
cp ~/.config/dsperate/dsperate.ini "$scratch/dsperate.ini"
for l in "$@"; do echo "$l" >> "$scratch/dsperate.ini"; done
timeout -s TERM $((secs + 1)) env SDL_VIDEODRIVER=kmsdrm SDL_KMSDRM_ATOMIC=0 XDG_RUNTIME_DIR="/run/user/$(id -u)" \
  DS_VERBOSE=1 DS_FPS=1 DS_FRAME_STATS=1 DS_AUDIO_QUEUE="$scratch/queue.txt" \
  "${DSPERATE:-dsperate}" --config "$scratch/dsperate.ini" --no-autoload ${STATE:+--load-state "$STATE"} --saves "$scratch/saves" --states "$scratch/saves" \
  --stats-from 300 "$rom" > "$scratch/log.txt" 2>&1
grep -a -E "^present: |^audio: queue depth|^frame budget|frameskip \(|frames not drawn" "$scratch/log.txt"
grep -a "fps (" "$scratch/log.txt" | awk '{f+=$1; n++; if ($1 < 59) low++} END {printf "mean %.1f fps, %d of %d seconds under 59\n", f/n, low, n}'
# Seconds in which the queue ran dry, from the per-frame log (column 4: the dry count so far).
awk 'NR % 60 == 1 { if (NR > 1 && $4 > last) gaps++; last = $4 } END { printf "seconds with a gap: %d\n", gaps }' "$scratch/queue.txt"
