# shellcheck shell=bash
# Shared start of the bench scripts that run on a computer: the settings
# (config.sh) and a few helpers. Source it by its path from the script:
#
#   . "$(cd "$(dirname "$0")" && pwd)/../lib.sh"
BENCH_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
export BENCH_ROOT
# shellcheck source=config.sh
. "$BENCH_ROOT/config.sh"
ssh_opts=(-o IdentitiesOnly=yes -i "$SSH_KEY")

die() { echo "${0##*/}: $*" >&2; exit 1; }
# The script's header comment (line 2 up to the first non-comment line), as usage.
usage() { awk 'NR > 1 && !/^#/ { exit } NR > 1 { sub(/^# ?/, ""); print }' "$0" >&2; exit "${1:-2}"; }
# shellcheck disable=SC2029 # the commands are built here on purpose
on_device() { ssh "${ssh_opts[@]}" "$DEVICE" "$@"; }
to_device() { scp -q "${ssh_opts[@]}" "$@"; }   # to_device FILE... DEVICE:PATH
# shellcheck disable=SC2029 # the commands are built here on purpose
# A command on the build machine (a login shell there), or here with BUILD_HOST=local.
on_build() { if [ "$BUILD_HOST" = local ]; then bash -c "$*"; else ssh "$BUILD_HOST" "$*"; fi; }
from_build() { if [ "$BUILD_HOST" = local ]; then cp "$1" "$2"; else scp -q "$BUILD_HOST:$1" "$2"; fi; }

case "${1:-}" in -h | --help) usage 0 ;; esac
