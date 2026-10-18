#!/bin/bash
# On the handheld, on battery: what a run costs in power. Runs COMMAND for its length and reports the
# battery's charge used (rk817's coulomb counter, charge_now, uAh), the mean of the gauge's 1 s
# current and voltage averages (current_avg, voltage_avg) and their product, and where the CPUs
# (cpufreq policy0, all four A55s) and the GPU (devfreq) spent the time by frequency.
# Refuses on the charger: the counter then measures charging.
#
#   bench/render/power-run.sh COMMAND [ARGS...]     (e.g. through device-run.sh, around render-bench.sh)
B=/sys/class/power_supply/rk817-battery
C=/sys/devices/system/cpu/cpufreq/policy0/stats/time_in_state
G=/sys/class/devfreq/fde60000.gpu/trans_stat
if [ "$(cat /sys/class/power_supply/rk817-charger/online)" != 0 ]; then
  echo "power-run: on the charger; unplug it to measure" >&2; exit 76
fi
cpu() { cat $C; }
gpu() { awk 'NR > 2 && NF > 2 {gsub(/\*/, ""); print $1, $NF}' $G; }
cpu > /tmp/pw-c0; gpu > /tmp/pw-g0; q0=$(cat $B/charge_now); t0=$(date +%s.%N)
( while :; do echo "$(cat $B/current_avg) $(cat $B/voltage_avg)"; sleep 1; done ) > /tmp/pw-samples &
sampler=$!
"$@"
kill $sampler
q1=$(cat "$B/charge_now"); t1=$(date +%s.%N)
cpu > /tmp/pw-c1; gpu > /tmp/pw-g1
awk -v q0="$q0" -v q1="$q1" -v t0="$t0" -v t1="$t1" '{i += $1; v += $2; p += $1 * $2 / 1e12; n++}
  END {printf "power: %.0f s, charge used %d uAh (%.0f mA mean); gauge samples: %.0f mA, %.3f V, %.2f W\n",
       t1 - t0, q0 - q1, (q0 - q1) * 3.6 / (t1 - t0), i / n / 1000, v / n / 1e6, p / n}' /tmp/pw-samples
join /tmp/pw-c0 /tmp/pw-c1 | awk '{d = $3 - $2; t += d; a[NR] = $1; b[NR] = d} END {printf "cpu time by MHz:"; for (k = 1; k <= NR; k++) if (b[k] * 100 >= t) printf " %d:%.0f%%", a[k] / 1000, 100 * b[k] / t; printf "\n"}'
join /tmp/pw-g0 /tmp/pw-g1 | awk '{d = $3 - $2; t += d; a[NR] = $1; b[NR] = d} END {printf "gpu time by MHz:"; if (!t) {printf " (no change of state: at %d MHz throughout)\n", a[NR] / 1e6; exit}; for (k = 1; k <= NR; k++) if (b[k] * 100 >= t) printf " %d:%.0f%%", a[k] / 1e6, 100 * b[k] / t; printf "\n"}'
