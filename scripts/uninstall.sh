#!/bin/bash
# Removes the S0ixSleep daemon and disables sleep again. Remove the kext from your EFI separately.
# Usage: sudo ./uninstall.sh
[ "$(id -u)" = 0 ] || { echo "run with sudo"; exit 1; }
LABEL=io.github.vikenzo196.s0ixsleep
launchctl bootout system/$LABEL 2>/dev/null
rm -f /Library/LaunchDaemons/$LABEL.plist /usr/local/bin/s0ix-arm.sh
sysctl -w debug.s0ix.enable=0 >/dev/null 2>&1
pmset -a disablesleep 1
echo "removed; sleep disabled (configuration kept in /usr/local/etc/s0ixsleep.conf)"
