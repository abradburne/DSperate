# shellcheck shell=sh
# Settings for the bench scripts that run on a computer and drive the handheld
# (pgo-train.sh, gba-exact.sh and the copies to the device). Don't edit it for
# your setup: put your values in bench/config.local.sh (not in git; see
# config.local.sh.example), or set them in the environment for one run:
#
#   DEVICE=me@handheld.local bench/core/pgo-train.sh HEAD
#
# Precedence: the environment, then config.local.sh, then the defaults here.
# Scripts that run on the handheld itself take their few paths (ROMS, ...)
# from the environment, with the defaults in their headers.

# shellcheck disable=SC1091 # optional, found at run time
[ -n "${BENCH_ROOT:-}" ] && [ -f "$BENCH_ROOT/config.local.sh" ] && . "$BENCH_ROOT/config.local.sh"

# The handheld: an ssh target, and the key to log in with.
: "${DEVICE:=user@handheld.local}"
: "${SSH_KEY:=$HOME/.ssh/id_ed25519}"
# Where builds for the handheld run: an ssh target of an aarch64 machine with
# the build tools (the same compiler as the package), or "local" to build here.
: "${BUILD_HOST:=local}"
# The home directory on BUILD_HOST and on the handheld must be the same path
# for PGO training (the instrumented binary writes its profile there).
: "${PGO_TRAIN_DIR:=/home/${DEVICE%%@*}/pgo-train}"
# GBA ROMs for gba-exact.sh (every .gba in it).
: "${GBA_ROMS:=$HOME/gba-roms}"
