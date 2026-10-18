#!/bin/bash
# On the handheld: run one measurement with the launcher (DSlot, a systemd
# service) out of the way, and start it again however the command ends.
# While a game is running (someone is playing) it refuses (exit 75), or with
# WAIT=1 waits until the game ends.
#
#   [WAIT=1] [LAUNCHER=dslot] device-run.sh COMMAND [ARGS...]     (sudo for systemctl)
launcher=${LAUNCHER:-dslot}
while pgrep -x dsperate > /dev/null; do
  [ -n "$WAIT" ] || { echo "device-run: a game is running; not stopping $launcher" >&2; exit 75; }
  echo "device-run: a game is running; waiting" >&2
  sleep 30
done
trap 'sudo systemctl start "$launcher"' EXIT
sudo systemctl stop "$launcher"
"$@"
