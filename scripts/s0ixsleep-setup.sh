#!/bin/bash
# S0ixSleep setup assistant.
#
#   ./s0ixsleep-setup.sh check    look at this Mac and tell whether S0ixSleep fits it (changes nothing, no sudo)
#
# Works with the tools that come with macOS (bash 3.2, ioreg, nvram, diskutil, kmutil, pmset).
set -u

OC_GUID=4D1FDA02-38C7-4A6A-9CC6-4BCCA8B30102
LABEL=io.github.vikenzo196.s0ixsleep
KEXT_ID=io.github.vikenzo196.S0ixSleep
LILU_ID=as.vit9696.Lilu
CONF=/usr/local/etc/s0ixsleep.conf
KILL=/usr/local/etc/s0ixsleep-off
REPO=https://github.com/vikenzo196/S0ixSleep

if [ -t 1 ]; then G=$'\033[32m'; Y=$'\033[33m'; R=$'\033[31m'; B=$'\033[1m'; N=$'\033[0m'; else G=; Y=; R=; B=; N=; fi
FAILS=0; WARNS=0
ok()      { printf '  %s[ ok ]%s %s\n' "$G" "$N" "$*"; }
warn()    { printf '  %s[warn]%s %s\n' "$Y" "$N" "$*"; WARNS=$((WARNS + 1)); }
fail()    { printf '  %s[fail]%s %s\n' "$R" "$N" "$*"; FAILS=$((FAILS + 1)); }
info()    { printf '  [info] %s\n' "$*"; }
more()    { printf '         %s\n' "$*"; }
section() { printf '\n%s%s%s\n' "$B" "$*" "$N"; }

# vge A B: true when version A >= version B (dotted numbers)
vge() { [ "$(printf '%s\n%s\n' "$2" "$1" | sort -t. -k1,1n -k2,2n -k3,3n | head -1)" = "$2" ]; }

KEXTS=
kext_version() { printf '%s\n' "$KEXTS" | awk -v id="$1" '$6 == id { gsub(/[()]/, "", $7); print $7; exit }'; }

ACPI=
# acpi_devices REGEX: ACPI names of the devices whose _HID/_CID matches REGEX
acpi_devices() {
	printf '%s\n' "$ACPI" | awk -v re="$1" '
		/\+-o / { n = $0; sub(/.*\+-o /, "", n); sub(/ .*/, "", n) }
		/"(name|compatible)" = / && $0 ~ re { print n }' | sort -u | tr '\n' ' ' | sed 's/ $//'
}

# esp_of_opencore: "diskXsY internal|external" of the partition OpenCore booted from
esp_of_opencore() {
	local bp uuid p u
	bp=$(nvram "$OC_GUID:boot-path" 2>/dev/null | cut -f2-)
	uuid=$(printf '%s' "$bp" | sed -n 's/.*HD([0-9]*,GPT,\([0-9A-Fa-f-]*\),.*/\1/p' | tr a-f A-F)
	[ -n "$uuid" ] || return 1
	for p in $(diskutil list 2>/dev/null | grep -oE 'disk[0-9]+s[0-9]+$'); do
		u=$(diskutil info -plist "$p" 2>/dev/null | plutil -extract DiskUUID raw - 2>/dev/null | tr a-f A-F)
		if [ "$u" = "$uuid" ]; then
			if [ "$(diskutil info -plist "$p" | plutil -extract Internal raw - 2>/dev/null)" = true ]; then
				echo "$p internal"
			else
				echo "$p external"
			fi
			return 0
		fi
	done
	return 1
}

stat_field() { printf '%s\n' "$STAT" | tr ' ' '\n' | sed -n "s/^$1=//p" | head -1; }

cmd_check() {
	local v major cpu sub esp oc ocver lilu s0ix pep lid ec bat gpu gfx sd hib route enable state

	printf '%sS0ixSleep check%s: looks at this Mac, changes nothing.\n' "$B" "$N"
	KEXTS=$(kmutil showloaded --list-only 2>/dev/null)
	ACPI=$(ioreg -p IOACPIPlane -lw0 2>/dev/null)

	section "macOS and CPU"
	v=$(sw_vers -productVersion); major=${v%%.*}
	if [ "$major" -ge 13 ] && [ "$major" -le 26 ]; then ok "macOS $v ($(sw_vers -buildVersion))"
	else warn "macOS $v: S0ixSleep is tested on macOS 13 to 26; on other versions it may refuse to hook (route=3)"; fi

	cpu=$(sysctl -n machdep.cpu.brand_string 2>/dev/null)
	if [ "$(sysctl -n machdep.cpu.vendor 2>/dev/null)" = GenuineIntel ]; then ok "$cpu"
	else fail "$cpu: S0ixSleep needs an Intel CPU"; fi

	sub=$(sysctl -n machdep.cpu.mwait.sub_Cstates 2>/dev/null || echo 0)
	if sysctl -n machdep.cpu.features 2>/dev/null | tr ' ' '\n' | grep -qx MON && [ $(((sub >> 28) & 15)) -gt 0 ] &&
		[ "$(sysctl -n machdep.cpu.thermal.invariant_APIC_timer 2>/dev/null)" = 1 ]; then
		ok "MWAIT C10 and an always-running APIC timer: the low-power wait (WAIT=2) can be used"
	else
		warn "no MWAIT C10 or no always-running APIC timer: S0ixSleep falls back to the TPAUSE wait (more power)"
	fi

	section "Boot loader"
	oc=$(nvram "$OC_GUID:opencore-version" 2>/dev/null | cut -f2-)
	if [ -n "$oc" ]; then
		ocver=$(printf '%s' "$oc" | awk -F- '{ v = $2; printf "%s.%s.%s (%s, %s-%s-%s)", substr(v,1,1), substr(v,2,1), substr(v,3), ($1 == "DBG" ? "debug" : "release"), $3, $4, $5 }')
		ok "OpenCore $ocver"
		if esp=$(esp_of_opencore); then
			info "OpenCore started from ${esp% *} (${esp#* } disk): that is the EFI to change"
		else
			warn "could not find the partition OpenCore started from"
		fi
	else
		fail "not started by OpenCore: S0ixSleep is a Lilu plugin for OpenCore setups"
	fi

	lilu=$(kext_version "$LILU_ID")
	if [ -z "$lilu" ]; then fail "Lilu is not loaded: add Lilu 1.6.0 or newer to your EFI first"
	elif vge "$lilu" 1.6.0; then ok "Lilu $lilu"
	else fail "Lilu $lilu: S0ixSleep needs Lilu 1.6.0 or newer"; fi

	section "Firmware"
	pep=$(acpi_devices '"(INT33A1|PNP0D80)"')
	if [ -n "$pep" ]; then
		ok "Modern Standby (low power S0) device: $pep"
		more "This firmware is built for Modern Standby. If macOS sleep resets, hangs or never wakes this"
		more "laptop, S3 does not work here and S0ixSleep is what it is for."
	else
		warn "no Modern Standby device (INT33A1/PNP0D80) found: the firmware probably supports S3,"
		more "so macOS sleep may already work without S0ixSleep. Try it first (with your work saved)."
	fi
	lid=$(acpi_devices '"PNP0C0D"')
	if [ -n "$lid" ]; then ok "lid: $lid"; else info "no lid device found: waking by opening the lid is not possible"; fi
	ec=$(acpi_devices '"PNP0C09"')
	if [ -n "$ec" ]; then
		ok "embedded controller: $ec"
		more "Its events (battery, AC, temperature...) wake the laptop until you list them in EC_IGNORE."
	else
		info "no embedded controller found"
	fi
	bat=$(pmset -g batt 2>/dev/null | grep -c InternalBattery)
	if [ "$bat" -gt 0 ]; then ok "internal battery"; else info "no internal battery: the battery guard has nothing to do"; fi

	section "Graphics"
	gpu=$(ioreg -lw0 -r -n IGPU -d 1 2>/dev/null | sed -n 's/.*"model" = <"\(.*\)">/\1/p' | head -1)
	[ -n "$gpu" ] || gpu=$(ioreg -lw0 -r -n GFX0 -d 1 2>/dev/null | sed -n 's/.*"model" = <"\(.*\)">/\1/p' | head -1)
	[ -n "$gpu" ] && info "integrated graphics: $gpu"
	gfx=$(printf '%s\n' "$KEXTS" | awk '$6 ~ /WhateverGreen|WhateverXe|Noot/ { gsub(/[()]/, "", $7); printf "%s %s  ", $6, $7 }')
	[ -n "$gfx" ] && info "graphics kexts: $gfx"
	more "The graphics driver must cope with a wake where the GPU kept its power (after S3 it would have"
	more "lost it). This is the main unknown on a new machine: see $REPO#graphics"

	section "Sleep now"
	s0ix=$(kext_version "$KEXT_ID")
	sd=$(pmset -g | awk '$1 == "SleepDisabled" { print $2 }')
	hib=$(pmset -g | awk '$1 == "hibernatemode" { print $2 }')
	if [ -n "$s0ix" ]; then
		STAT=$(sysctl -n debug.s0ix.stat 2>/dev/null)
		route=$(stat_field route); enable=$(stat_field enable)
		case "$route" in
			1) ok "S0ixSleep $s0ix loaded and hooked into macOS's sleep (route=1)" ;;
			2) fail "S0ixSleep $s0ix loaded, but acpi_sleep_kernel was not found (route=2): sleep stays disabled" ;;
			3) fail "S0ixSleep $s0ix loaded, but this macOS's sleep code is not the expected one (route=3): sleep stays disabled" ;;
			*) warn "S0ixSleep $s0ix loaded, not hooked yet (route=${route:-?})" ;;
		esac
		state=$(launchctl print "system/$LABEL" 2>/dev/null | awk '$1 == "state" { print $3; exit }')
		if [ -n "$state" ]; then ok "daemon $LABEL: $state"; else warn "daemon not installed: sleep stays disabled until it is"; fi
		[ -e "$KILL" ] && warn "kill switch $KILL is present: the daemon keeps sleep disabled"
		if [ "$enable" = 1 ] && [ "$sd" = 0 ]; then ok "S0ixSleep enabled and sleep allowed: ready"
		elif [ "$enable" = 1 ]; then warn "S0ixSleep enabled but sleep is disabled (SleepDisabled=$sd)"
		elif [ "$sd" = 0 ]; then fail "sleep is allowed but S0ixSleep is not enabled: the firmware's S3 would be used"
		else info "S0ixSleep not enabled, sleep disabled: safe"; fi
	else
		info "S0ixSleep is not installed"
		if [ "$sd" = 0 ] && [ -n "$pep" ]; then
			warn "sleep is allowed: on a Modern Standby laptop without working S3 it resets or hangs it."
			more "Until S0ixSleep is installed you can keep sleep off with:  sudo pmset -a disablesleep 1"
		else
			info "sleep disabled: ${sd:-unknown}"
		fi
	fi
	[ -n "$hib" ] && [ "$hib" != 0 ] && info "hibernatemode $hib (the daemon sets 0: hibernation also relies on S3)"

	section "Result"
	if [ "$FAILS" -gt 0 ]; then
		printf '  %sNot ready%s: fix the [fail] items above first.\n' "$R" "$N"
	elif [ -n "$s0ix" ]; then
		printf '  S0ixSleep is installed. Warnings: %s.\n' "$WARNS"
	elif [ -n "$pep" ]; then
		printf '  %sThis laptop looks like a match for S0ixSleep.%s Warnings: %s.\n' "$G" "$N" "$WARNS"
		printf '  Next: the installation steps in the README (%s#installation).\n' "$REPO"
	else
		printf '  %sS0ixSleep is probably not needed here%s: no Modern Standby device. Warnings: %s.\n' "$Y" "$N" "$WARNS"
	fi
	[ "$FAILS" -eq 0 ]
}

usage() {
	cat <<EOF
S0ixSleep setup assistant

  $0 check    look at this Mac and tell whether S0ixSleep fits it (changes nothing)
EOF
}

case "${1:-}" in
	check) cmd_check ;;
	-h|--help|help|"") usage ;;
	*) usage; exit 2 ;;
esac
