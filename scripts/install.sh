#!/bin/bash
# Installs the S0ixSleep daemon. The kext itself goes in your OpenCore EFI (see README).
# Usage: sudo ./install.sh
set -e
[ "$(id -u)" = 0 ] || { echo "run with sudo"; exit 1; }
cd "$(dirname "$0")"
LABEL=io.github.vikenzo196.s0ixsleep
pmset -a disablesleep 1
install -d -m 755 /usr/local/bin /usr/local/etc
install -m 755 s0ix-arm.sh /usr/local/bin/s0ix-arm.sh
[ -f /usr/local/etc/s0ixsleep.conf ] || install -m 644 s0ixsleep.conf /usr/local/etc/s0ixsleep.conf
install -m 644 -o root -g wheel $LABEL.plist /Library/LaunchDaemons/$LABEL.plist
launchctl bootout system/$LABEL 2>/dev/null || true
launchctl bootstrap system /Library/LaunchDaemons/$LABEL.plist
sleep 3
tail -3 /var/log/s0ixsleep.log
pmset -g | grep -E "SleepDisabled|hibernatemode"
