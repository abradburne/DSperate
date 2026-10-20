#!/bin/bash
# On the handheld: run one measurement with the launcher (DSlot, a systemd
# service) out of the way, and start it again however the command ends.
#
# The device is shared (other agents, scripts, the person playing), so it runs
# under the shared device lock: a directory on the device (DEVICE_LOCK, default
# /tmp/nibos-device.lock, nibOS's tools/device-lock.sh) with an `owner` file,
# taken here and dropped on exit. A caller that took it already (from another
# machine, `device-lock.sh take`) says so with LOCK_HELD=1. While the lock is
# someone else's, or a game is running (someone is playing), or the battery is
# under BATTERY_MIN percent (15), it refuses (exit 75); with WAIT=1 it waits
# for the lock or the game instead.
#
#   [WAIT=1] [LOCK_HELD=1] [WHO="me: what"] [LAUNCHER=dslot] device-run.sh COMMAND [ARGS...]   (sudo for systemctl)
launcher=${LAUNCHER:-dslot}
lock=${DEVICE_LOCK:-/tmp/nibos-device.lock}
who=${WHO:-"device-run.sh: $*"}
games='dsperate|inkdot|nibcase'

refuse() { echo "device-run: $1" >&2; exit 75; }

if [ -z "${LOCK_HELD:-}" ]; then
  until mkdir "$lock" 2>/dev/null; do
    [ -n "${WAIT:-}" ] || refuse "the device lock is held by: $(cat "$lock/owner" 2>/dev/null || echo '(unreadable)')"
    echo "device-run: waiting for the device lock ($(cat "$lock/owner" 2>/dev/null))" >&2
    sleep 30
  done
  printf '%s, %s\n' "$who" "$(date '+%a %d %b %H:%M:%S %Z')" > "$lock/owner"
  trap 'rm -rf "$lock"' EXIT
fi
while pgrep -x "$games" > /dev/null; do
  [ -n "${WAIT:-}" ] || refuse "a game is running; not stopping $launcher"
  echo "device-run: a game is running; waiting" >&2
  sleep 30
done
for cap in /sys/class/power_supply/*/capacity; do
  [ "$(cat "${cap%capacity}type" 2>/dev/null)" = Battery ] || continue
  [ "$(cat "$cap")" -ge "${BATTERY_MIN:-15}" ] || refuse "the battery is at $(cat "$cap")%; charge it first"
done
if [ -z "${LOCK_HELD:-}" ]; then
  trap 'sudo systemctl start "$launcher"; rm -rf "$lock"' EXIT
else
  trap 'sudo systemctl start "$launcher"' EXIT
fi
sudo systemctl stop "$launcher"
"$@"
