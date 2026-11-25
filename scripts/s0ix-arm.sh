#!/bin/bash
# s0ix-arm: enables S0ixSleep and only then lets macOS sleep.
# Runs as the LaunchDaemon io.github.vikenzo196.s0ixsleep (root). On firmware without a working S3, a sleep
# that is not handled by S0ixSleep resets or hangs the machine, so:
#  - at boot sleep is disabled at once (pmset disablesleep 1);
#  - the kext is configured from /usr/local/etc/s0ixsleep.conf and enabled (sysctl debug.s0ix.enable=1);
#  - sleep is allowed (disablesleep 0) only once the kext reports it is enabled;
#  - every minute this is checked again; at shutdown (SIGTERM) sleep is disabled again.
# While running it also watches the wakes from s0ix sleep (debug.s0ix.stat sleeps=):
#  - battery guard: on battery, a clean shutdown when the charge is at or below LOW_BATTERY_SLEEP % a few
#    seconds after a wake (SAFETY in s0ixsleep.conf wakes the machine in the dark periodically for this), or
#    at or below LOW_BATTERY_AWAKE % at any time;
#  - bag guard: after a wake, if nobody touched the keyboard or trackpad and the display is off for
#    BAG_GUARD seconds, back to sleep (a wake nobody asked for must not keep the laptop running in a bag);
#  - network off while asleep: the interfaces in SLEEP_IF_DOWN go down while the lid is closed and no display
#    is on (so not in clamshell mode with an external monitor), and come back when the lid opens or a display
#    turns on. Off by default: on the tested machine (itlwm) it made no measurable difference.
# Kill switch: create /usr/local/etc/s0ixsleep-off and reboot (sleep stays disabled).
export PATH=/usr/sbin:/usr/bin:/bin:/sbin
CONF=/usr/local/etc/s0ixsleep.conf
LOG=/var/log/s0ixsleep.log
log() { echo "$(date '+%F %T') $*" >> "$LOG"; }
safe() { pmset -a disablesleep 1; log "$1: sleep disabled"; }
idle() { while :; do sleep 3600 & wait $!; done; }
trap 'safe "shutdown"; [ "${net_down:-0}" = 1 ] && for i in $SLEEP_IF_DOWN; do if_set "$i" up; done; exit 0' TERM INT

pmset -a disablesleep 1
log "start"
if [ -f /usr/local/etc/s0ixsleep-off ]; then log "s0ixsleep-off present, not enabling"; idle; fi
for i in $(seq 1 120); do sysctl -n debug.s0ix.stat 2>/dev/null | grep -q "^route=1 " && break; sleep 1; done
if ! sysctl -n debug.s0ix.stat 2>/dev/null | grep -q "^route=1 "; then
  log "S0ixSleep not loaded or its route failed ($(sysctl -n debug.s0ix.stat 2>/dev/null | cut -d' ' -f1)): sleep stays disabled"
  idle
fi

# Configuration: KEY=value lines, see s0ixsleep.conf.
WAIT=2; PERIOD=50; SAFETY=3600; GT_RESET=0; EC_IGNORE=; EC_WAKE=; EC_REPLAY=1; WAKE_GPE=
LOW_BATTERY_SLEEP=10; LOW_BATTERY_AWAKE=4; BAG_GUARD=60; SLEEP_IF_DOWN=
[ -f "$CONF" ] && . "$CONF"
sysctl -w debug.s0ix.period="$PERIOD" >/dev/null
if ! sysctl -w debug.s0ix.wait="$WAIT" >/dev/null 2>&1; then
  log "wait $WAIT refused (CPU or APIC not suitable), using wait 1"
  sysctl -w debug.s0ix.wait=1 >/dev/null
fi
sysctl -w debug.s0ix.safety="$SAFETY" >/dev/null
sysctl -w debug.s0ix.gtreset="$GT_RESET" >/dev/null
sysctl -w debug.s0ix.ec_replay="$EC_REPLAY" >/dev/null 2>&1
[ -n "$EC_IGNORE" ] && sysctl -w debug.s0ix.ec_ignore="$EC_IGNORE" >/dev/null
[ -n "$EC_WAKE" ] && sysctl -w debug.s0ix.ec_wake="$EC_WAKE" >/dev/null
[ -n "$WAKE_GPE" ] && sysctl -w debug.s0ix.wake_gpe="$WAKE_GPE" >/dev/null 2>&1

# Hibernation and standby would take another path to S3/S4.
pmset -a hibernatemode 0 standby 0 autopoweroff 0 >/dev/null 2>&1

if sysctl -w debug.s0ix.enable=1 >/dev/null 2>&1 && [ "$(sysctl -n debug.s0ix.enable)" = 1 ]; then
  pmset -a disablesleep 0
  log "enabled (wait $(sysctl -n debug.s0ix.wait)), sleep allowed: $(sysctl -n debug.s0ix.stat | cut -d' ' -f1-12)"
else
  log "enable failed (no FADT event blocks?): sleep stays disabled: $(sysctl -n debug.s0ix.stat 2>/dev/null | cut -d' ' -f1-8)"
  idle
fi
stat() { sysctl -n debug.s0ix.stat 2>/dev/null; }
field() { stat | tr ' ' '\n' | sed -n "s/^$1=//p"; }
# "<percent> <BAT|AC>"
battery() {
  local b; b=$(pmset -g batt 2>/dev/null)
  printf '%s %s\n' "$(printf '%s' "$b" | grep -oE '[0-9]+%' | head -1 | tr -d '%')" \
    "$(printf '%s' "$b" | grep -q "'Battery Power'" && echo BAT || echo AC)"
}
check_battery() {   # $1 threshold, $2 label
  local pct src; read -r pct src <<< "$(battery)"
  if [ "$src" = BAT ] && [ -n "$pct" ] && [ "$pct" -le "$1" ]; then
    log "battery $pct% <= $1% ($2): clean shutdown"
    shutdown -h now; idle
  fi
}
hid_idle() { ioreg -c IOHIDSystem -d 4 -r -k HIDIdleTime 2>/dev/null | awk '/HIDIdleTime/ {print int($NF / 1000000000); exit}'; }
display_on() { pmset -g powerstate IODisplayWrangler 2>/dev/null | awk '$1 == "IODisplayWrangler" {exit !($2 >= 4)}'; }
# Lid closed and the lid makes the machine sleep (macOS sets AppleClamshellCausesSleep No with an external display).
lid_closed() {
  ioreg -rc IOPMrootDomain -d1 2>/dev/null | awk -F'= ' '/"AppleClamshellState"/ {c = ($2 == "Yes")} /"AppleClamshellCausesSleep"/ {s = ($2 == "Yes")} END {exit !(c && s)}'
}
# Native Wi-Fi through networksetup, anything else (itlwm, Ethernet) through ifconfig.
if_set() {   # $1 interface, $2 up|down
  if networksetup -listallhardwareports 2>/dev/null |
     awk -v d="$1" '/^Hardware Port:/ {p = $0} $0 == "Device: " d {f = (p ~ /Wi-Fi|AirPort/)} END {exit !f}'; then
    networksetup -setairportpower "$1" "$([ "$2" = up ] && echo on || echo off)"
  else
    ifconfig "$1" "$2"
  fi
}
net_down=0

last=$(field sleeps); woke=0; checked=1; guard=0; n=0
while :; do
  sleep 2 & wait $!
  n=$((n + 1))
  if [ $((n % 30)) = 0 ]; then
    if [ "$(sysctl -n debug.s0ix.enable 2>/dev/null)" != 1 ]; then safe "S0ixSleep disabled"; idle; fi
    check_battery "$LOW_BATTERY_AWAKE" awake
  fi
  if [ -n "$SLEEP_IF_DOWN" ]; then
    if [ "$net_down" = 0 ] && lid_closed && ! display_on; then
      for i in $SLEEP_IF_DOWN; do if_set "$i" down; done; net_down=1; log "lid closed, display off: $SLEEP_IF_DOWN down"
    elif [ "$net_down" = 1 ] && { ! lid_closed || display_on; }; then
      for i in $SLEEP_IF_DOWN; do if_set "$i" up; done; net_down=0; log "lid open or display on: $SLEEP_IF_DOWN up"
    fi
  fi
  s=$(field sleeps)
  if [ -n "$s" ] && [ "$s" != "$last" ]; then
    last=$s; woke=$(date +%s); checked=0; guard=1
    log "wake $s: reason $(field reason), $(field ms) ms, package $(field pkg_mw) mW, EC $(field ec_q), replayed $(field replayed)"
  fi
  [ "$woke" = 0 ] && continue
  since=$(( $(date +%s) - woke ))
  if [ "$checked" = 0 ] && [ "$since" -ge 5 ]; then checked=1; check_battery "$LOW_BATTERY_SLEEP" "after sleep"; fi
  # /var/run/s0ixsleep-keepawake (removed at reboot) holds the bag guard off, e.g. for remote or scripted work.
  if [ "$guard" = 1 ] && [ "$BAG_GUARD" -gt 0 ] 2>/dev/null && [ ! -e /var/run/s0ixsleep-keepawake ]; then
    idle_s=$(hid_idle)
    # Input in the first 10 s belongs to the wake itself (key, lid, or the event that woke it).
    if [ -n "$idle_s" ] && [ "$since" -ge 10 ] && [ "$idle_s" -lt $((since - 10)) ]; then guard=0   # in use
    elif [ "$since" -ge "$BAG_GUARD" ] && ! display_on; then
      guard=0; log "bag guard: no input for $since s since the wake, display off: back to sleep"
      pmset sleepnow >/dev/null
    fi
  fi
done
