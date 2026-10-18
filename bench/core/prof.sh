#!/bin/bash
# bench/core/prof.sh (on the handheld): perf profile of one bench scene, with JIT blocks named through
# DS_PERF_MAP. Uses bench.sh's scene table.
#
#   prof.sh BIN SCENE OUT        (bench.sh next to it)
set -euo pipefail
[ $# -eq 3 ] || { sed -n '2,/^set/{/^#/s/^# \{0,1\}//p;}' "$0" >&2; exit 2; }
BIN=$1 SC=$2 OUT=$3
mkdir -p "$OUT"
# bench.sh runs this wrapper in place of the emulator; BIN, OUT and SC are filled in here.
cat > /tmp/prof-wrap.sh <<W
#!/bin/bash
exec perf record -q -F 1999 -o "$OUT/$SC.data" -- "$BIN" "\$@"
W
chmod +x /tmp/prof-wrap.sh
DS_PERF_MAP=1 "$(dirname "$0")/bench.sh" /tmp/prof-wrap.sh "$OUT/run" "$SC" >/dev/null
perf report -i "$OUT/$SC.data" --no-children --sort comm -q 2>/dev/null | head -12 > "$OUT/$SC.comm"
perf report -i "$OUT/$SC.data" --no-children --sort comm,sym -q 2>/dev/null | head -70 > "$OUT/$SC.sym"
