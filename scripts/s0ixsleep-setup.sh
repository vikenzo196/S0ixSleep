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

	FAILS=0; WARNS=0
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

# ---------------------------------------------------------------------------------------------------------
# install / uninstall

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
PB=/usr/libexec/PlistBuddy
DRY=0; YES=0; NODAEMON=0; EFI_DIR=; KEXT_SRC=
ROOT=; EFI_DESC=; MOUNTED_BY_US=; TMP=

parse_opts() {
	while [ $# -gt 0 ]; do
		case "$1" in
			--dry-run) DRY=1 ;;
			--yes|-y) YES=1 ;;
			--no-daemon) NODAEMON=1 ;;
			--efi) [ $# -ge 2 ] || { usage; exit 2; }; EFI_DIR=$2; shift ;;
			--kext) [ $# -ge 2 ] || { usage; exit 2; }; KEXT_SRC=$2; shift ;;
			*) usage; exit 2 ;;
		esac
		shift
	done
}

cleanup() {
	[ -n "$TMP" ] && rm -rf "$TMP"
	[ -n "$MOUNTED_BY_US" ] && diskutil unmount "$MOUNTED_BY_US" >/dev/null 2>&1
	TMP=; MOUNTED_BY_US=
}
trap cleanup EXIT
die()  { printf '  %s[stop]%s %s\n' "$R" "$N" "$*"; exit 1; }
step() { printf '\n%s%s%s\n' "$B" "$*" "$N"; }
ask() {
	local a
	if [ "$YES" = 1 ]; then printf '%s [y/N] y\n' "$1"; return 0; fi
	printf '%s [y/N] ' "$1"
	{ read -r a </dev/tty; } 2>/dev/null || return 1
	case "$a" in y|Y|yes|Yes|YES) return 0 ;; *) return 1 ;; esac
}

need_root() {
	if [ -n "$EFI_DIR" ] && [ "$NODAEMON" = 1 ]; then return; fi
	[ "$(id -u)" = 0 ] || die "run it with sudo:  sudo $0 $*"
}

user_home() {
	if [ -n "${SUDO_USER:-}" ] && [ "$SUDO_USER" != root ]; then
		dscl . -read "/Users/$SUDO_USER" NFSHomeDirectory 2>/dev/null | awk '{ print $2 }'
	else
		echo "$HOME"
	fi
}

# find_efi: sets ROOT (the folder holding EFI/) and EFI_DESC; mounts the ESP if needed
find_efi() {
	local esp dev mp
	if [ -n "$EFI_DIR" ]; then
		ROOT=$(cd "$EFI_DIR" 2>/dev/null && pwd) || die "no such folder: $EFI_DIR"
		EFI_DESC="folder $ROOT"
	else
		esp=$(esp_of_opencore) || die "could not find the partition OpenCore started from (use --efi FOLDER)"
		dev=${esp% *}
		mp=$(diskutil info -plist "$dev" | plutil -extract MountPoint raw - 2>/dev/null)
		if [ -z "$mp" ]; then
			diskutil mount "$dev" >/dev/null 2>&1 || die "could not mount $dev"
			MOUNTED_BY_US=$dev
			mp=$(diskutil info -plist "$dev" | plutil -extract MountPoint raw - 2>/dev/null)
		fi
		ROOT=$mp
		EFI_DESC="$dev (${esp#* } disk), mounted at $mp"
	fi
	[ -f "$ROOT/EFI/OC/config.plist" ] || die "no EFI/OC/config.plist in $ROOT"
}

# scan_config FILE: sets NKEXT, LILU_IDX, LILU_EN, S0IX_IDX, S0IX_EN
scan_config() {
	local bp
	NKEXT=0; LILU_IDX=; LILU_EN=; S0IX_IDX=; S0IX_EN=
	while bp=$($PB -c "Print :Kernel:Add:$NKEXT:BundlePath" "$1" 2>/dev/null); do
		case "$bp" in
			Lilu.kext) LILU_IDX=$NKEXT; LILU_EN=$($PB -c "Print :Kernel:Add:$NKEXT:Enabled" "$1" 2>/dev/null) ;;
			S0ixSleep.kext) S0IX_IDX=$NKEXT; S0IX_EN=$($PB -c "Print :Kernel:Add:$NKEXT:Enabled" "$1" 2>/dev/null) ;;
		esac
		NKEXT=$((NKEXT + 1))
	done
}

add_entry() { # add_entry FILE INDEX EXECUTABLE
	local e=":Kernel:Add:$2"
	$PB -c "Add $e dict" -c "Add $e:Arch string x86_64" -c "Add $e:BundlePath string S0ixSleep.kext" \
		-c "Add $e:Comment string S0ixSleep (sleep without S3)" -c "Add $e:Enabled bool true" \
		-c "Add $e:ExecutablePath string Contents/MacOS/$3" -c "Add $e:MaxKernel string" \
		-c "Add $e:MinKernel string" -c "Add $e:PlistPath string Contents/Info.plist" "$1"
}

# backup_efi WHAT...: copies the files named (relative to EFI/OC) and a zip of EFI/OC to ~/S0ixSleep-backup/DATE
backup_efi() {
	local home f
	home=$(user_home)
	BK="$home/S0ixSleep-backup/$(date +%Y%m%d-%H%M%S)"
	mkdir -p "$BK" || die "cannot create $BK"
	for f in "$@"; do
		[ -e "$ROOT/EFI/OC/$f" ] && ditto --norsrc --noextattr "$ROOT/EFI/OC/$f" "$BK/$(basename "$f")"
	done
	ditto -c -k --norsrc --keepParent "$ROOT/EFI/OC" "$BK/EFI-OC.zip" || die "backup of EFI/OC failed"
	printf 'EFI: %s\nDate: %s\nRestore: copy config.plist back to EFI/OC/, or unzip EFI-OC.zip over EFI/\n' \
		"$EFI_DESC" "$(date)" > "$BK/README.txt"
	[ -n "${SUDO_USER:-}" ] && chown -R "$SUDO_USER" "$home/S0ixSleep-backup" 2>/dev/null
	ok "backup in $BK"
}

copy_kext() {
	local dst="$ROOT/EFI/OC/Kexts/S0ixSleep.kext"
	rm -rf "$dst.new"
	ditto --norsrc --noextattr --noqtn --noacl "$KEXT_SRC" "$dst.new" || die "could not copy the kext"
	rm -rf "$dst" && mv "$dst.new" "$dst" || die "could not replace $dst"
}

kext_info() { $PB -c "Print :$2" "$1/Contents/Info.plist" 2>/dev/null; }

cmd_install() {
	local kver kexe dst dver kact cact dact free ocv
	need_root install
	printf '%sS0ixSleep install%s%s\n' "$B" "$N" "$([ "$DRY" = 1 ] && echo ' (dry run: nothing is written)')"

	step "1/4  Check"
	cmd_check >/dev/null 2>&1; local rc=$?
	if [ "$rc" -ne 0 ]; then
		cmd_check
		die "the check found problems: fix the [fail] items first"
	fi
	ok "this Mac passes the check (details: $0 check)"
	if [ -z "$(acpi_devices '"(INT33A1|PNP0D80)"')" ]; then
		warn "no Modern Standby device: this firmware probably has working S3"
		ask "Install anyway?" || die "nothing changed"
	fi

	step "2/4  What will change"
	[ -n "$KEXT_SRC" ] || KEXT_SRC="$SCRIPT_DIR/../S0ixSleep.kext"
	[ "$(kext_info "$KEXT_SRC" CFBundleIdentifier)" = "$KEXT_ID" ] || die "S0ixSleep.kext not found next to this script (use --kext PATH)"
	KEXT_SRC=$(cd "$KEXT_SRC" && pwd)
	kver=$(kext_info "$KEXT_SRC" CFBundleShortVersionString); kexe=$(kext_info "$KEXT_SRC" CFBundleExecutable)
	[ -f "$KEXT_SRC/Contents/MacOS/$kexe" ] || die "the kext at $KEXT_SRC has no executable"

	find_efi
	scan_config "$ROOT/EFI/OC/config.plist"
	[ -n "$LILU_IDX" ] || die "Lilu.kext is not in config.plist > Kernel > Add"
	[ "$LILU_EN" = true ] || die "Lilu.kext is disabled in config.plist"

	dst="$ROOT/EFI/OC/Kexts/S0ixSleep.kext"
	if [ -d "$dst" ]; then
		dver=$(kext_info "$dst" CFBundleShortVersionString)
		if diff -rq "$KEXT_SRC" "$dst" >/dev/null 2>&1; then kact=same; else kact=replace; fi
	else
		kact=new
	fi
	if [ -z "$S0IX_IDX" ]; then cact=add
	elif [ "$S0IX_IDX" -lt "$LILU_IDX" ]; then cact=move
	elif [ "$S0IX_EN" != true ]; then cact=enable
	else cact=none; fi
	if [ "$NODAEMON" = 1 ]; then dact=skip
	elif [ -f "/Library/LaunchDaemons/$LABEL.plist" ]; then dact=update
	else dact=new; fi

	info "EFI:     $EFI_DESC"
	case $kact in
		new)     info "kext:    copy S0ixSleep.kext $kver to EFI/OC/Kexts" ;;
		replace) info "kext:    replace S0ixSleep.kext ${dver:-?} in EFI/OC/Kexts with $kver" ;;
		same)    info "kext:    S0ixSleep.kext $kver already in EFI/OC/Kexts: no change" ;;
	esac
	case $cact in
		add)    info "config:  Kernel > Add: new entry for S0ixSleep.kext right after Lilu.kext" ;;
		move)   info "config:  Kernel > Add: move the S0ixSleep.kext entry after Lilu.kext" ;;
		enable) info "config:  Kernel > Add: enable the S0ixSleep.kext entry" ;;
		none)   info "config:  S0ixSleep.kext already listed after Lilu and enabled: no change" ;;
	esac
	case $dact in
		new)    info "daemon:  install $LABEL (sleep stays disabled until the kext runs)" ;;
		update) info "daemon:  update the daemon script (your /usr/local/etc/s0ixsleep.conf is kept)" ;;
		skip)   info "daemon:  skipped (--no-daemon)" ;;
	esac
	if [ "$kact" != same ] || [ "$cact" != none ]; then
		info "backup:  config.plist and a zip of EFI/OC go to ~/S0ixSleep-backup first"
	fi

	if [ "$DRY" = 1 ]; then printf '\nDry run: nothing written.\n'; return 0; fi
	if [ "$kact" = same ] && [ "$cact" = none ] && [ "$dact" = skip ]; then printf '\nNothing to do.\n'; return 0; fi
	echo
	ask "Go ahead?" || die "nothing changed"

	step "3/4  EFI"
	if [ "$kact" != same ] || [ "$cact" != none ]; then
		backup_efi config.plist Kexts/S0ixSleep.kext
		TMP=$(mktemp -d /tmp/s0ixsleep.XXXXXX)
		cp "$ROOT/EFI/OC/config.plist" "$TMP/config.plist"
		case $cact in
			add) add_entry "$TMP/config.plist" $((LILU_IDX + 1)) "$kexe" ;;
			move)
				$PB -c "Delete :Kernel:Add:$S0IX_IDX" "$TMP/config.plist"
				scan_config "$TMP/config.plist"
				add_entry "$TMP/config.plist" $((LILU_IDX + 1)) "$kexe" ;;
			enable) $PB -c "Set :Kernel:Add:$S0IX_IDX:Enabled true" "$TMP/config.plist" ;;
		esac
		plutil -lint -s "$TMP/config.plist" || die "the edited config.plist is not valid: the EFI was not changed"
		scan_config "$TMP/config.plist"
		[ -n "$S0IX_IDX" ] && [ "$S0IX_IDX" -gt "$LILU_IDX" ] && [ "$S0IX_EN" = true ] ||
			die "the edited config.plist does not look right: the EFI was not changed"
		ocv=$(command -v ocvalidate 2>/dev/null || true)
		[ -x "$SCRIPT_DIR/../Utilities/ocvalidate" ] && ocv="$SCRIPT_DIR/../Utilities/ocvalidate"
		if [ -n "$ocv" ]; then
			if "$ocv" "$TMP/config.plist" >/dev/null 2>&1; then ok "ocvalidate: no problems"
			else warn "ocvalidate reports problems (maybe already there before, or a different OpenCore version):"
				"$ocv" "$TMP/config.plist" 2>&1 | tail -5 | sed 's/^/         /'
				ask "Write the config anyway?" || die "the EFI was not changed"
			fi
		fi
		if [ "$kact" != same ]; then
			free=$(df -k "$ROOT" | awk 'NR == 2 { print $4 }')
			[ "${free:-0}" -gt 1024 ] || die "less than 1 MB free on the EFI partition"
			copy_kext
			ok "S0ixSleep.kext $kver copied"
		fi
		if [ "$cact" != none ]; then
			cp "$TMP/config.plist" "$ROOT/EFI/OC/config.plist" || die "could not write config.plist (backup in $BK)"
			ok "config.plist updated (S0ixSleep.kext is entry $S0IX_IDX, Lilu.kext entry $LILU_IDX)"
		fi
	else
		ok "EFI already up to date"
	fi

	step "4/4  Daemon"
	if [ "$dact" = skip ]; then
		info "skipped"
	else
		"$SCRIPT_DIR/install.sh" >/dev/null 2>&1 || die "the daemon install failed (sudo $SCRIPT_DIR/install.sh shows why)"
		ok "daemon $LABEL installed"
		info "sleep: $(pmset -g | awk '$1 == "SleepDisabled" { print ($2 == 1 ? "disabled until the kext runs" : "allowed") }')"
	fi

	step "Done"
	more "1. Reboot."
	more "2. Then run:  sudo $0 test   (a first sleep with a 60 s safety timer)"
	more "Optional: the ACPI part turns the fans and power LED off during sleep; it depends on the laptop."
	more "See docs/ACPI.md. Undo everything with:  sudo $0 uninstall"
}

cmd_uninstall() {
	local dst cact kact
	need_root uninstall
	printf '%sS0ixSleep uninstall%s%s\n' "$B" "$N" "$([ "$DRY" = 1 ] && echo ' (dry run: nothing is written)')"
	find_efi
	scan_config "$ROOT/EFI/OC/config.plist"
	dst="$ROOT/EFI/OC/Kexts/S0ixSleep.kext"
	if [ -n "$S0IX_IDX" ]; then cact=remove; else cact=none; fi
	if [ -d "$dst" ]; then kact=remove; else kact=none; fi

	step "What will change"
	info "EFI:     $EFI_DESC"
	[ "$cact" = remove ] && info "config:  remove the S0ixSleep.kext entry from Kernel > Add" || info "config:  no S0ixSleep.kext entry"
	[ "$kact" = remove ] && info "kext:    delete EFI/OC/Kexts/S0ixSleep.kext" || info "kext:    not in EFI/OC/Kexts"
	[ "$NODAEMON" = 1 ] && info "daemon:  skipped (--no-daemon)" || info "daemon:  remove it and disable sleep again (your s0ixsleep.conf is kept)"
	more "ACPI tables or patches you added for S0ixSleep (docs/ACPI.md) stay: remove them yourself if you want."
	if [ "$DRY" = 1 ]; then printf '\nDry run: nothing written.\n'; return 0; fi
	echo
	ask "Go ahead?" || die "nothing changed"

	if [ "$NODAEMON" != 1 ]; then
		"$SCRIPT_DIR/uninstall.sh" >/dev/null 2>&1
		ok "daemon removed, sleep disabled"
	fi
	if [ "$cact" = remove ] || [ "$kact" = remove ]; then
		backup_efi config.plist Kexts/S0ixSleep.kext
		if [ "$cact" = remove ]; then
			TMP=$(mktemp -d /tmp/s0ixsleep.XXXXXX)
			cp "$ROOT/EFI/OC/config.plist" "$TMP/config.plist"
			$PB -c "Delete :Kernel:Add:$S0IX_IDX" "$TMP/config.plist"
			plutil -lint -s "$TMP/config.plist" || die "the edited config.plist is not valid: the EFI was not changed"
			cp "$TMP/config.plist" "$ROOT/EFI/OC/config.plist" || die "could not write config.plist (backup in $BK)"
			ok "S0ixSleep.kext removed from config.plist"
		fi
		[ "$kact" = remove ] && rm -rf "$dst" && ok "EFI/OC/Kexts/S0ixSleep.kext deleted"
	fi
	step "Done"
	more "Reboot to unload the kext. Sleep stays disabled."
}

# ---------------------------------------------------------------------------------------------------------
# test / report

conf_get() { [ -f "$CONF" ] && sed -n "s/^$1=//p" "$CONF" | tail -1; }

# conf_set KEY VALUE: changes (or adds) KEY=VALUE in s0ixsleep.conf, keeping a dated copy
conf_set() {
	cp "$CONF" "$CONF.bak-$(date +%Y%m%d-%H%M%S)" || return 1
	if grep -q "^$1=" "$CONF"; then
		sed -i '' "s/^$1=.*/$1=$2/" "$CONF"
	else
		printf '%s=%s\n' "$1" "$2" >> "$CONF"
	fi
}

TEST_SAFETY=; TEST_HOLD=0
test_restore() {
	[ -n "$TEST_SAFETY" ] && sysctl -w debug.s0ix.safety="$TEST_SAFETY" >/dev/null 2>&1
	[ "$TEST_HOLD" = 1 ] && rm -f /var/run/s0ixsleep-keepawake
	TEST_SAFETY=; TEST_HOLD=0
}

describe_reason() { # describe_reason REASON ECGPE
	local r=$(($1)) e=$(($2))
	case $r in
		1) echo "the power button" ;;
		2) echo "an RTC alarm (a wake macOS scheduled, for example for maintenance)" ;;
		3) echo "the 60 s safety timer: nothing else woke it" ;;
		4) echo "the lid (its state read from the embedded controller, LID_EC)" ;;
		*) if [ $r -ge 256 ] && [ $((r - 256)) -eq "$e" ]; then echo "an embedded controller event"
		   elif [ $r -ge 256 ]; then printf 'general purpose event 0x%x (lid, keyboard, USB or another device the OS armed)\n' $((r - 256))
		   else echo "unknown ($1)"; fi ;;
	esac
}

cmd_test() {
	local s0ix route enable sd before after t0 reason ecgpe q ms pkg mw ign
	[ "$(id -u)" = 0 ] || die "run it with sudo:  sudo $0 test"
	printf '%sS0ixSleep test%s: one sleep with a 60 s safety timer.\n' "$B" "$N"
	KEXTS=$(kmutil showloaded --list-only 2>/dev/null)
	s0ix=$(kext_version "$KEXT_ID")
	[ -n "$s0ix" ] || die "S0ixSleep is not loaded: install it ($0 install) and reboot first"
	STAT=$(sysctl -n debug.s0ix.stat 2>/dev/null)
	route=$(stat_field route); enable=$(stat_field enable)
	[ "$route" = 1 ] || die "S0ixSleep is not hooked (route=$route): see $0 check"
	[ "$enable" = 1 ] || die "S0ixSleep is not enabled by the daemon: see $0 check"
	sd=$(pmset -g | awk '$1 == "SleepDisabled" { print $2 }')
	[ "$sd" = 0 ] || die "sleep is disabled (SleepDisabled=$sd): see $0 check"

	more "The laptop goes to sleep and wakes by itself after 60 s; this script then turns the display on."
	more "Leave it alone to see whether something else wakes it, or wake it earlier with a key or the lid."
	more "Save your work first. Do not close this window."
	echo
	ask "Start?" || die "nothing changed"

	TEST_SAFETY=$(sysctl -n debug.s0ix.safety 2>/dev/null)
	trap 'test_restore; cleanup' EXIT
	if [ ! -e /var/run/s0ixsleep-keepawake ]; then touch /var/run/s0ixsleep-keepawake; TEST_HOLD=1; fi
	sysctl -w debug.s0ix.safety=60 >/dev/null || die "could not set the safety timer"
	before=$(stat_field sleeps)
	t0=$(date +%s)
	printf '  going to sleep (macOS takes 10-30 s to get there)...\n'
	pmset sleepnow >/dev/null
	while :; do
		sleep 1
		STAT=$(sysctl -n debug.s0ix.stat 2>/dev/null); after=$(stat_field sleeps)
		[ "${after:-0}" -gt "${before:-0}" ] && break
		if [ $(($(date +%s) - t0)) -gt 120 ]; then
			test_restore
			warn "macOS did not go to sleep within 2 minutes. What kept it awake:"
			pmset -g assertions | grep -E "PreventSystemSleep|PreventUserIdleSystemSleep|NoIdleSleep" | sed 's/^/         /' | head -8
			die "test not done"
		fi
	done
	caffeinate -u -t 3 >/dev/null 2>&1 &
	sleep 2
	test_restore
	STAT=$(sysctl -n debug.s0ix.stat 2>/dev/null)
	reason=$(stat_field reason); ecgpe=$(stat_field ecgpe); ms=$(stat_field ms); pkg=$(stat_field pkg_mw)
	mw=$(stat_field mwait); q=$(stat_field ec_q | cut -d, -f1)

	step "Result"
	ok "slept for $((${ms:-0} / 1000)) s and woke up"
	info "woken by $(describe_reason "${reason:-0}" "${ecgpe:-0}")"
	[ -n "$pkg" ] && [ "$pkg" -gt 0 ] && info "CPU package power while asleep: $(awk -v m="$pkg" 'BEGIN { printf "%.2f W", m / 1000 }')"
	[ "$mw" = 1 ] && ok "low-power wait (MWAIT C10) used" || info "low-power wait not used (mwait=${mw:-?}, mwait_fail=$(stat_field mwait_fail))"

	if [ $((reason)) -ge 256 ] && [ $((reason - 256)) -eq $((ecgpe)) ] && [ "$q" != 1ff ]; then
		echo
		more "The embedded controller woke it with event $q (method _Q$(echo "$q" | tr a-f A-F) in your DSDT)."
		if ask "Did you open the lid, press a key or touch the laptop just before it woke?"; then
			more "Then event $q may be the lid or the keyboard: keep it as a wake event."
		else
			more "Nobody asked for this wake: battery, AC and temperature events do this. Ignoring event $q"
			more "means it no longer wakes the laptop (it still runs after the next wake, with EC_REPLAY=1)."
			if [ -n "$(conf_get EC_WAKE)" ]; then
				info "your s0ixsleep.conf uses EC_WAKE (only listed events wake): edit it by hand"
			elif ask "Add $q to EC_IGNORE in $CONF?"; then
				ign=$(conf_get EC_IGNORE)
				ign=$(printf '%s\n' "$ign" | tr ',' '\n' | grep -v "^$" | { cat; echo "$q"; } | sort -u | paste -sd, -)
				conf_set EC_IGNORE "$ign" || die "could not change $CONF"
				sysctl -w debug.s0ix.ec_ignore="$ign" >/dev/null 2>&1
				ok "EC_IGNORE=$ign (saved and active now)"
				more "Run the test again: other events may show up one at a time."
			fi
		fi
	elif [ $((reason)) -eq 3 ]; then
		echo
		more "Now try the lid: close it, wait 15 s, open it. The laptop should wake in a few seconds."
		more "If the lid sometimes does not wake it, see LID_EC in $CONF."
	fi
	echo
	more "If the display came back wrong or black, see $REPO#graphics"
	more "Status line (for a report):"
	more "$(sysctl -n debug.s0ix.stat 2>/dev/null)"
}

cmd_report() {
	local out serial uuid user f
	user=${SUDO_USER:-$(id -un)}
	out="$(user_home)/Desktop/S0ixSleep-report-$(date +%Y%m%d-%H%M%S).txt"
	serial=$(ioreg -c IOPlatformExpertDevice -d 2 | sed -n 's/.*"IOPlatformSerialNumber" = "\(.*\)"/\1/p' | head -1)
	uuid=$(ioreg -c IOPlatformExpertDevice -d 2 | sed -n 's/.*"IOPlatformUUID" = "\(.*\)"/\1/p' | head -1)
	{
		echo "S0ixSleep report, $(date)"
		echo; echo "== check"; cmd_check 2>&1 | sed 's/\x1b\[[0-9;]*m//g'
		echo; echo "== debug.s0ix"; sysctl debug.s0ix 2>&1
		echo; echo "== kexts"; kmutil showloaded --list-only 2>/dev/null | awk '$6 !~ /^com\.apple\./ { print $6, $7 }'
		echo; echo "== boot-args"; nvram boot-args 2>&1
		echo; echo "== pmset -g"; pmset -g 2>&1
		echo; echo "== s0ixsleep.conf"; grep -v '^#' "$CONF" 2>/dev/null | grep -v '^$'
		echo; echo "== s0ixsleep.log (last 60 lines)"; tail -60 /var/log/s0ixsleep.log 2>/dev/null
		echo; echo "== sleep and wake (pmset -g log, last 40)"; pmset -g log 2>/dev/null | grep -E " (Sleep|Wake|DarkWake) " | tail -40
		echo; echo "== kernel messages"; dmesg 2>/dev/null | grep -iE "s0ix|lilu" | tail -40
	} | sed -e "s#/Users/$user#~#g" | { if [ -n "$serial" ]; then sed "s/$serial/<serial>/g"; else cat; fi; } |
		{ if [ -n "$uuid" ]; then sed "s/$uuid/<uuid>/g"; else cat; fi; } > "$out"
	[ -n "${SUDO_USER:-}" ] && chown "$SUDO_USER" "$out" 2>/dev/null
	printf 'Report written to %s\n' "$out"
	more "Serial number and hardware UUID are removed. Read it before you post it, then attach it to an issue:"
	more "$REPO/issues"
}

usage() {
	cat <<EOF
S0ixSleep setup assistant

  $0 check        look at this Mac and tell whether S0ixSleep fits it (changes nothing)
  sudo $0 install      put the kext in your OpenCore EFI and install the daemon, step by step
  sudo $0 test         after the reboot: one sleep with a 60 s safety timer, explained
  sudo $0 report       write a report for an issue (serial number removed) to the Desktop
  sudo $0 uninstall    remove the daemon, the kext and its config.plist entry

Options for install and uninstall:
  --dry-run       show what would change, write nothing
  --efi FOLDER    use FOLDER/EFI instead of the EFI partition OpenCore started from
  --kext PATH     use this S0ixSleep.kext (default: the one next to the scripts folder)
  --no-daemon     leave the daemon alone (EFI only)
  --yes           do not ask for confirmation
EOF
}

cmd="${1:-}"; [ $# -gt 0 ] && shift
case "$cmd" in
	check) cmd_check ;;
	install) parse_opts "$@"; cmd_install ;;
	uninstall) parse_opts "$@"; cmd_uninstall ;;
	test) parse_opts "$@"; cmd_test ;;
	report) cmd_report ;;
	-h|--help|help|"") usage ;;
	*) usage; exit 2 ;;
esac
