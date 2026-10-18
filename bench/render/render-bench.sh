#!/bin/bash
# On the handheld: DSperate's render and present cost, one game from boot (no auto slot, no input),
# both panels, real-time priority as under dslot.service, the installed settings plus the
# overrides given. Frame-skip off, so every frame is drawn and timed. Prints the emulation's
# frame times, the present thread's job times, frames not shown, fps, and with hi-res 3D the
# present thread's CPU time per render (DS_DEBUG=hires-stats); STAGES=1 adds the GPU time per stage
# (hires-stages, timer queries: they change the batching, so not with the rest). Builds from before
# DS_DEBUG read DS_HIRES_DEBUG / DS_HIRES_STAGES, which are set too. PERF=<seconds> also records the whole process with perf for that
# long, starting 15 s in, and prints the top symbols per thread. See docs/rk3568/render-review.md.
#
# STATE=<file.dss> starts from that save state instead of boot. FALLBACK=1 leaves the automatic 2x -> 1x
# fallback (video.hires3d_fallback) on; off by default, so the GPU's cost is measured at 2x throughout.
#
#   [BIN=dsperate] [HIRES=1|2] [PERF=20] [STATE=f.dss] bench/render/render-bench.sh ROM SECONDS [ini lines...]
#   (as the handheld's user, DSlot stopped; sudo for the real-time limit and perf)
rom=$1 secs=$2
shift 2
bin=${BIN:-dsperate}
scratch=/tmp/render-bench
rm -rf "$scratch" && mkdir -p "$scratch/saves"
cp ~/.config/dsperate/dsperate.ini "$scratch/dsperate.ini"
{ echo "[video]"; echo "hires3d = ${HIRES:-1}"; [ -z "$FALLBACK" ] && echo "hires3d_fallback = false"; echo "[emu]"; echo "frameskip = 0"; for l in "$@"; do echo "$l"; done; } >> "$scratch/dsperate.ini"
sudo prlimit --pid $$ --rtprio=10:10
env SDL_VIDEODRIVER=kmsdrm SDL_KMSDRM_ATOMIC=0 XDG_RUNTIME_DIR="/run/user/$(id -u)" \
  DS_VERBOSE=1 DS_FPS=1 DS_FRAME_STATS=1 DS_DEBUG="hires-stats${STAGES:+,hires-stages}" DS_HIRES_DEBUG=1 ${STAGES:+DS_HIRES_STAGES=1} \
  timeout -s TERM $((secs + 1)) "$bin" --config "$scratch/dsperate.ini" --no-autoload \
  --saves "$scratch/saves" --states "$scratch/saves" ${STATE:+--load-state "$STATE"} --stats-from 300 "$rom" > "$scratch/log.txt" 2>&1 &
if [ -n "$PERF" ]; then
  sleep 15
  pid=$(pgrep -n -f "^$bin|/$bin ")
  sudo perf record -F 999 -p "$pid" -o "$scratch/perf.data" -- sleep "$PERF" > /dev/null 2>&1
fi
wait
log=$scratch/log.txt
grep -a -E "^frame ms|^present thread|^present: " "$log"
grep -a "fps (" "$log" | tail -n +6 | awk '{f+=$1; n++; if ($1 < 59) low++; split($0,a,"emu "); e+=a[2]+0; split($0,b,"present "); p+=b[2]+0; split($0,c,"wait "); w+=c[2]+0}
  END {printf "fps %.1f mean, %d of %d s under 59; per frame: emu %.2f, present %.2f, wait %.2f ms\n", f/n, low, n, e/n, p/n, w/n}'
grep -a "vsync lock:" "$log" | awk '{l+=$3; o+=$6; c+=$10; r+=$16} END {if (NR) printf "vsync lock, summed: %d late flips, %d presents left out, %d frames run to catch up, %d restarts\n", l, o, c, r}'
avg() {   # the mean of each "name value" field of lines starting with $1, after the first such line
  grep -a "^$1" "$log" | tail -n +2 | sed -e "s/^$1[^:]*: *//" -e 's/ *(.*//' | awk -F', *' -v t="$2" '
    {for (i = 1; i <= NF; i++) {k = $i; v = $i; sub(/ [^ ]+$/, "", k); sub(/.* /, "", v); s[k] += v; if (!(k in o)) o[k] = ++m; key[o[k]] = k}; n++}
    END {if (n) {printf "%s:", t; for (j = 1; j <= m; j++) printf " %s %.2f%s", key[j], s[key[j]] / n, j < m ? "," : ""; printf "\n"}}'
}
avg "gpu ms" "gpu ms a render"
avg "hires3d cpu ms" "present cpu ms a render"
grep -a "^hires3d: .*frames:" "$log" | tail -1
if [ -n "$PERF" ]; then
  sudo perf report -i "$scratch/perf.data" --no-children --sort comm,sym --percent-limit 0.7 --stdio 2> /dev/null | grep -v "^#" | grep -v "^$" | head -60
fi
