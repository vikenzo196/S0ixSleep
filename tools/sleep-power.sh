#!/bin/bash
# Measures the whole-machine power during an s0ix sleep, on battery, with the lid open (run with sudo).
# The machine sleeps for DUR seconds (S0ixSleep's safety timer wakes it), then the drain is computed from the
# battery gauge. Gauges that move in 1 % steps are read at the moment of a step before the sleep and of a real
# step after the wake; the awake drain in between is the integral of InstantAmperage sampled every second.
# Usage: sudo tools/sleep-power.sh [DUR=1800] [LABEL]   (OUTDIR=dir for the CSV, NET_IF=en0 to take an interface down)
export PATH=/usr/sbin:/usr/bin:/bin:/sbin
DUR=${1:-1800}; LABEL=${2:-test}; NET_IF=${NET_IF:-}
DIR=${OUTDIR:-.}
CSV=$DIR/prova-$LABEL-$(date +%m%d-%H%M).csv
bat() { ioreg -rn AppleSmartBattery 2>/dev/null | awk '
  $1 == "\"CurrentCapacity\"" {c = $3} $1 == "\"Voltage\"" {v = $3} $1 == "\"InstantAmperage\"" {a = $3}
  $1 == "\"ExternalConnected\"" {e = $3} END {if (a > 9e18) a = a - 18446744073709551616; print c, v, a, e}'; }
sleeps() { sysctl -n debug.s0ix.stat | tr ' ' '\n' | sed -n 's/^sleeps=//p'; }
say() { echo "$(date +%T) $*"; }

read -r c v a e <<< "$(bat)"
[ "$e" = No ] || { say "on AC power: abort"; exit 1; }
pct=$(pmset -g batt | grep -oE '[0-9]+%' | head -1 | tr -d %)
[ "${pct:-0}" -gt 15 ] || { say "battery ${pct}%: abort"; exit 1; }

# sampler: epoch, capacity, mV, mA, sleeps (a gap in the epochs = the sleep)
( while :; do read -r c v a e <<< "$(bat)"; echo "$(date +%s),$c,$v,$a,$(sleeps)"; sleep 1; done ) > "$CSV" &
SAMPLER=$!
OLD_SAFETY=$(sysctl -n debug.s0ix.safety); KEEP=0; [ -e /var/run/s0ixsleep-keepawake ] && KEEP=1
trap 'kill $SAMPLER 2>/dev/null; [ -n "$NET_IF" ] && ifconfig "$NET_IF" up; [ "$KEEP" = 0 ] && rm -f /var/run/s0ixsleep-keepawake; sysctl -w debug.s0ix.safety="$OLD_SAFETY" >/dev/null' EXIT

# a gauge step before the sleep (at most 6 minutes)
c0=$(bat | cut -d' ' -f1); n=0
while [ "$(bat | cut -d' ' -f1)" = "$c0" ] && [ $n -lt 360 ]; do sleep 1; n=$((n + 1)); done
say "step before sleep: $c0 -> $(bat | cut -d' ' -f1) mAh"
s0=$(sleeps)
[ -n "$NET_IF" ] && ifconfig "$NET_IF" down
touch /var/run/s0ixsleep-keepawake   # the bag guard must not end the measurement
sysctl -w debug.s0ix.safety="$DUR" >/dev/null
say "sleep for $DUR s"
pmset sleepnow
while [ "$(sleeps)" = "$s0" ]; do sleep 1; done
caffeinate -d -u -t 900 & CAF=$!
say "woke: $(sysctl -n debug.s0ix.stat | tr ' ' '\n' | grep -E '^(reason|ms|pkg_mw|pc)=' | tr '\n' ' ')"
sysctl -w debug.s0ix.safety="$OLD_SAFETY" >/dev/null
[ -n "$NET_IF" ] && ifconfig "$NET_IF" up
# after the wake the gauge first refreshes (not a real step), then a real step (at most 8 minutes)
sleep 15; c1=$(bat | cut -d' ' -f1); n=0
while [ "$(bat | cut -d' ' -f1)" = "$c1" ] && [ $n -lt 480 ]; do sleep 1; n=$((n + 1)); done
say "step after wake: $c1 -> $(bat | cut -d' ' -f1) mAh"
sleep 2
kill $SAMPLER 2>/dev/null; kill $CAF 2>/dev/null
echo "$(sysctl -n debug.s0ix.stat)" > "${CSV%.csv}.stat"
python3 "$(dirname "$0")/sleep-power.py" "$CSV"
