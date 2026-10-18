#!/bin/bash
# bench/core/gba-exact.sh (any machine that runs the builds): run GBA ROMs headless with two dsperate-headless builds and
# compare their frame hashes and audio; prints the time per ROM for each.
#
#   bench/core/gba-exact.sh BASE NEW [frames]
#
# The ROMs are every .gba in $GBA_ROMS (bench/config.sh).
set -euo pipefail
[ $# -ge 2 ] || { sed -n '2,/^set/{/^#/s/^# \{0,1\}//p;}' "$0" >&2; exit 2; }
B=$1 N=$2 F=${3:-3600}
# shellcheck source-path=SCRIPTDIR source=../lib.sh
. "$(cd "$(dirname "$0")" && pwd)/../lib.sh"
R=$GBA_ROMS
O=$(mktemp -d /tmp/gba-exact.XXXXXX)
P=()
for f in $(seq 400 150 3000); do P+=(--press "$f:start:4" --press "$((f + 75)):a:4"); done
for rom in "$R"/*.gba; do
    n=$(basename "$rom" .gba)
    [ ${#n} -le 12 ] && continue   # the short names are duplicates
    for w in base new; do
        bin=$B
        [ "$w" = new ] && bin=$N
        s=$(date +%s.%N)
        "$bin" --gba --frames "$F" "${P[@]}" --hash-frames "$O/$w.hash" --dump-audio "$O/$w.pcm" "$rom" > "$O/$w.log" 2>&1 ||
            echo "$n: $w exited with $? (see $O/$w.log)" >&2
        e=$(date +%s.%N)
        printf -v "t_$w" '%s' "$(echo "$e - $s" | bc)"
    done
    fh=$(cmp -s "$O/base.hash" "$O/new.hash" && echo same || echo DIFF)
    ah=$(cmp -s "$O/base.pcm" "$O/new.pcm" && echo same || echo DIFF)
    # shellcheck disable=SC2154 # t_base and t_new are set by printf -v above
    printf "%-45s frames %s audio %s  %.2fs -> %.2fs\n" "${n:0:45}" "$fh" "$ah" "$t_base" "$t_new"
done
