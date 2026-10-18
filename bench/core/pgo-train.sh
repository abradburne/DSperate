#!/bin/bash
# A PGO profile for the handheld build, trained on the handheld (run on a computer).
#
#   bench/core/pgo-train.sh COMMIT [scene...]
#
# Builds COMMIT instrumented (DSPERATE_PGO=generate) on BUILD_HOST with the
# package's flags (packaging/arch/PKGBUILD), runs core/bench.sh's scenes with it
# on DEVICE (the launcher stopped through tools/device-run.sh, WAIT=1), and
# writes the .gcda files and a MANIFEST into pgo/aarch64-gcc<version>/, which a
# build with that compiler picks up (DSPERATE_PGO=use; CMakeLists.txt). The
# profile belongs to that compiler and those flags (the configure refuses
# others); edited functions just lose theirs, so retrain for each release.
# Measured on the RG DS+: 2-13% off the median frame time on the trained scenes,
# 1-7% on games left out (docs/rk3568/core-review.md). Settings: bench/config.sh.
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
# shellcheck source-path=SCRIPTDIR source=../lib.sh
. "$HERE/../lib.sh"
COMMIT=${1:-}
[ -n "$COMMIT" ] || usage
shift
SCENES=("$@"); [ ${#SCENES[@]} -gt 0 ] || SCENES=(gsboot mkds ac gs1 nsmb0 aos dkc2)
SRC=$(cd "$HERE/../.." && pwd)
PROF=$PGO_TRAIN_DIR
FLAGS="-DCMAKE_BUILD_TYPE=Release -DDSPERATE_TUNE=-mcpu=cortex-a55 -DDSPERATE_NET=OFF -DDSPERATE_CHEEVOS=OFF -DDSPERATE_TESTS=OFF"
work=pgo-train   # on BUILD_HOST, under its home (or the current directory, locally)

git -C "$SRC" archive "$COMMIT" | on_build "rm -rf $work-src $work-build && mkdir -p $work-src && tar -x -C $work-src && \
  cmake -S $work-src -B $work-build -G Ninja $FLAGS -DDSPERATE_PGO=generate -DDSPERATE_PGO_DIR=$PROF > /dev/null && \
  cmake --build $work-build --target dsperate-headless > /dev/null"
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
from_build "$work-build/src/frontend/headless/dsperate-headless" "$tmp/pgogen"
fp=$(on_build "cat $work-build/pgo-fingerprint")
gcc=$(on_build "g++ -dumpfullversion")
OUT=${PGO_OUT:-$SRC/pgo/aarch64-gcc$gcc}
on_device "mkdir -p ~/dsperate-bench/bin && rm -rf $PROF"
to_device "$tmp/pgogen" "$DEVICE:dsperate-bench/bin/pgogen"
to_device "$HERE/bench.sh" "$HERE/../tools/device-run.sh" "$DEVICE:dsperate-bench/"
on_device "cd ~/dsperate-bench && WAIT=1 ./device-run.sh ./bench.sh bin/pgogen out/pgotrain ${SCENES[*]}"
rm -rf "$OUT" && mkdir -p "$OUT"
scp -q "${ssh_opts[@]}" "$DEVICE:$PROF/*.gcda" "$OUT/"
{ echo "fingerprint $fp"
  echo "compiler $gcc"
  echo "commit $(git -C "$SRC" rev-parse "$COMMIT")"
  echo "date $(date -u +%Y-%m-%dT%H:%MZ)"
  echo "scenes ${SCENES[*]} (bench/core/bench.sh, on the handheld)"; } > "$OUT/MANIFEST"
echo "profile: $(find "$OUT" -type f | wc -l | tr -d ' ') files in $OUT"
