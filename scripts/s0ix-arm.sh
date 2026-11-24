#!/bin/bash
# s0ix-arm: enables S0ixSleep and only then lets macOS sleep.
# Runs as the LaunchDaemon io.github.vikenzo196.s0ixsleep (root). On firmware without a working S3, a sleep
# that is not handled by S0ixSleep resets or hangs the machine, so:
#  - at boot sleep is disabled at once (pmset disablesleep 1);
#  - the kext is configured from /usr/local/etc/s0ixsleep.conf and enabled (sysctl debug.s0ix.enable=1);
#  - sleep is allowed (disablesleep 0) only once the kext reports it is enabled;
#  - every minute this is checked again; at shutdown (SIGTERM) sleep is disabled again.
# Kill switch: create /usr/local/etc/s0ixsleep-off and reboot (sleep stays disabled).
export PATH=/usr/sbin:/usr/bin:/bin:/sbin
CONF=/usr/local/etc/s0ixsleep.conf
LOG=/var/log/s0ixsleep.log
log() { echo "$(date '+%F %T') $*" >> "$LOG"; }
safe() { pmset -a disablesleep 1; log "$1: sleep disabled"; }
idle() { while :; do sleep 3600 & wait $!; done; }
trap 'safe "shutdown"; exit 0' TERM INT

pmset -a disablesleep 1
log "start"
if [ -f /usr/local/etc/s0ixsleep-off ]; then log "s0ixsleep-off present, not enabling"; idle; fi
for i in $(seq 1 120); do sysctl -n debug.s0ix.stat 2>/dev/null | grep -q "^route=1 " && break; sleep 1; done
if ! sysctl -n debug.s0ix.stat 2>/dev/null | grep -q "^route=1 "; then
  log "S0ixSleep not loaded or its route failed ($(sysctl -n debug.s0ix.stat 2>/dev/null | cut -d' ' -f1)): sleep stays disabled"
  idle
fi

# Configuration: KEY=value lines, see s0ixsleep.conf.
WAIT=1; SAFETY=0; GT_RESET=0; EC_IGNORE=; EC_WAKE=
[ -f "$CONF" ] && . "$CONF"
sysctl -w debug.s0ix.wait="$WAIT" >/dev/null
sysctl -w debug.s0ix.safety="$SAFETY" >/dev/null
sysctl -w debug.s0ix.gtreset="$GT_RESET" >/dev/null
[ -n "$EC_IGNORE" ] && sysctl -w debug.s0ix.ec_ignore="$EC_IGNORE" >/dev/null
[ -n "$EC_WAKE" ] && sysctl -w debug.s0ix.ec_wake="$EC_WAKE" >/dev/null

# Hibernation and standby would take another path to S3/S4.
pmset -a hibernatemode 0 standby 0 autopoweroff 0 >/dev/null 2>&1

if sysctl -w debug.s0ix.enable=1 >/dev/null 2>&1 && [ "$(sysctl -n debug.s0ix.enable)" = 1 ]; then
  pmset -a disablesleep 0
  log "enabled, sleep allowed: $(sysctl -n debug.s0ix.stat | cut -d' ' -f1-12)"
else
  log "enable failed (no FADT event blocks?): sleep stays disabled: $(sysctl -n debug.s0ix.stat 2>/dev/null | cut -d' ' -f1-8)"
  idle
fi
while :; do
  sleep 60 & wait $!
  if [ "$(sysctl -n debug.s0ix.enable 2>/dev/null)" != 1 ]; then safe "S0ixSleep disabled"; idle; fi
done
