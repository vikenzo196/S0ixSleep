<h1 align="center">
  <picture>
    <source media="(prefers-color-scheme: dark)" srcset="docs/logo/wordmark-dark.svg">
    <img alt="S0ixSleep" src="docs/logo/wordmark-light.svg" width="393">
  </picture>
</h1>

<p align="center">Sleep for Hackintosh laptops whose firmware has no S3.</p>

Most laptops released since about 2020 only support Modern Standby (S0ix): the firmware no longer
implements S3 (suspend to RAM). macOS only knows S3, so on these machines closing the lid or choosing
Sleep resets the laptop, hangs it with a black screen, or never wakes. The usual advice is to disable
sleep altogether (`pmset disablesleep 1`).

S0ixSleep is a [Lilu](https://github.com/acidanthera/Lilu) plugin that lets macOS sleep anyway.

> **Experimental.** Tested on one machine so far (Samsung Galaxy Book3 Ultra, i9-13900H, macOS Tahoe 26.7).
> Reports from other laptops are very welcome.

## How it works

macOS keeps its whole sleep path: apps and drivers are told about sleep and power their devices down,
the other CPU cores are turned off and the firmware's `_PTS` runs. Only the very last step changes.
`acpi_sleep_kernel` ends with a callback that writes `SLP_TYP`/`SLP_EN` to enter S3; S0ixSleep replaces
that callback. Instead of entering S3, the last CPU core:

1. lowers its HWP performance request to the minimum;
2. sleeps in `MWAIT` C10 and wakes every 50 ms on its local APIC timer;
3. each time, checks the ACPI event registers (power button, RTC alarm, and every general purpose event
   the OS armed for wake: lid, embedded controller, ...);
4. on a wake event, puts the CPU state back and returns. XNU then runs its normal resume path, exactly as
   after a sleep attempt that failed, and macOS wakes up.

XNU has already shut the core's local APIC down at that point, so no interrupt would end a deep `MWAIT`.
S0ixSleep enables the APIC for the wait, points its timer (TSC deadline mode) at a vector of a copy of the
kernel's IDT whose gate only acknowledges it, and raises the task priority so that no kernel interrupt can
be delivered; afterwards everything is put back exactly as it was. With the waiting core in C7 the
package can leave its active state. (Fallback `wait 1`: `TPAUSE` C0.2, simpler but the package stays in
PC0.) The I/O ports of the event registers come from the FADT; the embedded controller's ports come from
the ECDT (default `0x62`/`0x66`).

After a wake caused by the user (key, lid, power button), S0ixSleep asks the power manager for a full wake
(`IOPMrootDomain::requestUserActive`), otherwise macOS would stay in a dark wake with the display off.

**This is not real S0ix.** macOS has no driver for the Intel PMC, so the package does not reach the deep
S0ix states that Windows uses. Devices are powered down by their macOS drivers and the CPU waits at its
lowest level, but the machine draws more than in Windows Modern Standby: on the tested laptop about 4.9 W, so
a full battery lasts about half a day asleep. It is meant for closing the lid and moving around, or a night
on a charged battery, not for leaving a laptop asleep in a bag for days.

## Results (Galaxy Book3 Ultra)

| | |
|---|---|
| Wake | lid open, any key, power button; display back in about 2 s |
| Whole machine while asleep (battery gauge, `tools/sleep-power.sh`) | about 4.9 W (66 Wh battery: about 13 h) |
| CPU package while asleep (RAPL) | about 1.8 W with `wait 2` (package in PC3 98% of the time); 2.9 W with `wait 1` |
| Fans, power LED | off (with the SSDT that tells the EC about Modern Standby) |
| Time to fall asleep | about 17 s, mostly powerd's own dark wake delay |

The package stops at PC3 on this machine because its display engine stays on (see
[Package idle states](#package-idle-states)); taking the Wi-Fi card down during the sleep made no measurable
difference, and macOS already turns the NVMe controller off at every sleep.

## Requirements

- OpenCore, [Lilu](https://github.com/acidanthera/Lilu) 1.6.0 or newer.
- Intel CPU. HWP and the RAPL power reading need an Intel CPU; the wait itself is plain ACPI.
- macOS 13 to 26. S0ixSleep checks the code of `acpi_sleep_kernel` before it hooks it; if the check
  fails (`route=3` in the status), nothing is changed and sleep stays disabled.
- An ACPI setup where macOS can already reach sleep: if your machine resets when it sleeps, that is the
  right starting point.
- A graphics driver that survives a wake where the GPU **kept** its power (after S3 it would have lost
  it). This is the main unknown on other machines; see [Graphics](#graphics).

## Installation

The setup assistant does it step by step and shows every change before it makes it:

```
cd S0ixSleep-x.y.z
./scripts/s0ixsleep-setup.sh check           # does S0ixSleep fit this Mac? (changes nothing)
sudo ./scripts/s0ixsleep-setup.sh install    # kext into your OpenCore EFI (after a backup), then the daemon
# reboot
sudo ./scripts/s0ixsleep-setup.sh test       # a first sleep with a 60 s safety timer, explained
```

- `install` finds the EFI partition OpenCore started from, backs up `config.plist` and the whole `EFI/OC`
  folder to `~/S0ixSleep-backup`, copies the kext and adds it to Kernel → Add right after Lilu.
  `--dry-run` shows the changes without writing anything.
- `test` tells what woke the laptop. If an embedded controller event woke it while nobody touched it, it
  offers to add that event to `EC_IGNORE` (see [Spurious wakes](#spurious-wakes-embedded-controller-events)).
- `report` writes a report for an issue to the Desktop, with the serial number removed.
- `uninstall` removes the daemon, the kext and its `config.plist` entry.

The ACPI part ([docs/ACPI.md](docs/ACPI.md)) depends on the laptop and stays manual.

### By hand

1. Add `S0ixSleep.kext` to `EFI/OC/Kexts` and to `config.plist` → Kernel → Add, **after Lilu**.
2. Optional but recommended: the ACPI part, see [docs/ACPI.md](docs/ACPI.md).
3. Reboot, then install the daemon:

   ```
   cd S0ixSleep-x.y.z/scripts
   sudo ./install.sh
   ```

   The daemon `io.github.vikenzo196.s0ixsleep` disables sleep at boot, configures and enables the kext from
   `/usr/local/etc/s0ixsleep.conf`, and **only then** allows sleep. If the kext is missing or cannot be
   enabled, sleep stays disabled, so the machine never falls back to the firmware's broken S3.
   It also sets `hibernatemode 0`, `standby 0` and `autopoweroff 0`.

4. First test, with a 60 s safety timer: set `SAFETY=60` in `/usr/local/etc/s0ixsleep.conf`, run
   `sudo launchctl kickstart -k system/io.github.vikenzo196.s0ixsleep`, then `pmset sleepnow`.
   The laptop should wake by itself after a minute, or earlier when you press a key.

Kill switch: `sudo touch /usr/local/etc/s0ixsleep-off` and reboot. Uninstall: `sudo ./uninstall.sh`.

## Protections (daemon)

macOS cannot see the battery while it sleeps this way (the embedded controller's battery events are not
handled during the wait), and a wake nobody asked for would leave the laptop running in a closed bag. The
daemon therefore watches every wake from s0ix sleep (all settings in `s0ixsleep.conf`):

- **Hourly dark wake** (`SAFETY=3600`): the machine wakes in the dark (display off, about 45 s) and goes back
  to sleep, so the checks below also run during a long sleep.
- **Battery guard**: on battery, a clean `shutdown` when the charge is at or below `LOW_BATTERY_SLEEP` (10 %)
  a few seconds after a wake, or `LOW_BATTERY_AWAKE` (4 %) at any time. A drained battery would otherwise
  end in a hard power loss.
- **Bag guard** (`BAG_GUARD=60`): after a wake, if nobody touched the keyboard or trackpad and the display is
  off for 60 s, back to sleep. `/var/run/s0ixsleep-keepawake` (removed at reboot) holds it off for remote work.
- **EC event replay** (`EC_REPLAY=1`): after a wake, the firmware's `_Qxx` methods of the EC events ignored
  during the sleep run once, so macOS sees battery and AC changes at once.
- Optional `SLEEP_IF_DOWN=en0`: network interfaces down while the lid is closed and no display is on.

## Status and tuning

```
sysctl debug.s0ix.stat
```

| Field | Meaning |
|---|---|
| `route` | 1 = hooked. 2 = symbol not found, 3 = unexpected code (macOS version not supported) |
| `platform` | 1 = FADT read. `pm1`, `gpe0` (port/bytes), `ec` (command/data ports), `ecgpe`: what it found |
| `sleeps`, `reason` | completed sleeps and last wake reason: 1 power button, 2 RTC, 3 safety timer, 4 lid opened (state read from the EC, `LID_EC`), `0x100` + GPE number |
| `ms`, `pkg_mw`, `core_mw` | length of the last sleep, average package and core power during it |
| `mwait`, `timer_irqs`, `core_c7_ms`, `mwait_fail` | 1 = last sleep used `wait 2`; timer wakes; core C7 time; why `wait 2` was not entered (1 APIC mode changed, 2 IDT) |
| `ec_q`, `ec_ign`, `replayed` | last embedded controller events read during sleep (newest first), events ignored, `_Qxx` replayed after the last wake / in total |
| `userwake` | full wakes requested after a user wake |
| `hooks` | wait hooks added by other kexts / calls (see [Graphics](#graphics)) |
| `lid_ec`, `lid` | lid state location in EC RAM (`LID_EC`); its value at sleep entry > last value read / read errors |
| `wake_gpe`, `gpe_sts` | extra wake GPEs (`WAKE_GPE`); GPE0 status at sleep entry > at wake |
| `pc`, `res_ok` | package C-state residency of the last sleep, per mille (PC2,3,6,7,8,9,10) |

### Lid that does not always wake the laptop

Opening the lid normally wakes the laptop through an embedded controller event (a `_Qxx` method that notifies
the lid device). Some ECs do not always send it after a long sleep. `LID_EC` makes S0ixSleep read the lid state
from the EC's RAM every 500 ms during the sleep and wake when it changes from closed to open (`reason` 4):

```
LID_EC=83.0      # EC RAM byte 0x83, bit 0, 1 = open (~83.0 if 0 = open)
```

Find it in your DSDT: the field of the EC's `EmbeddedControl` region that the lid's `_Qxx` method copies into
the variable `_LID` returns (on the Samsung Galaxy Book3 it is `LSTE`, `Offset (0x83)`, bit 0). Each read is a
short EC transaction; the measured package power did not change.

### Spurious wakes: embedded controller events

The embedded controller raises one GPE for all its events: battery level, AC adapter, temperature, fans,
as well as the lid and the keyboard. By default every EC event wakes the laptop. If it wakes up by itself,
`reason` is `0x100` + the EC's GPE and `ec_q` shows the event number (the `_Qxx` method in your DSDT).
Look the methods up in your DSDT and list the ones that should not wake in `s0ixsleep.conf`:

```
EC_IGNORE=51,52,53,54     # these events do not wake
# or
EC_WAKE=50,5e,5f          # only these events wake
```

A filtered event is consumed: its `_Qxx` method cannot run during the wait; with `EC_REPLAY=1` (default) it
runs once after the next wake. The kext reads the event only when no keyboard byte is waiting in the 8042,
so key presses always wake.

The settings are also sysctls (`debug.s0ix.ec_ignore`, `debug.s0ix.ec_wake`, `debug.s0ix.safety`,
`debug.s0ix.wait`, `debug.s0ix.period`, `debug.s0ix.gtreset`) and boot arguments (`s0ixecign=51,52`,
`s0ixecwake=5e,5f`).

### Package idle states

`pc=` in the status is the share of the last sleep (per mille) the package spent in PC2, PC3, PC6, PC7,
PC8, PC9 and PC10; `core_c7_ms` is the time the waiting core spent in C7. If the package stops at PC2/PC3,
something outside the CPU cores keeps it awake. On the tested machine it is the display engine: its pipe
and eDP link stay on during sleep, because the graphics driver cannot use panel self refresh or the
display's DC5/DC6 states. With `sudo sysctl debug.s0ix.pmc_diag=1` S0ixSleep also snapshots the PCH power
management controller around each sleep (latency tolerance of every PCH device and of the whole platform,
power-gated blocks, SLP_S0 conditions); `python3 tools/pmc_decode.py` decodes `sysctl debug.s0ix.pmc`
(Intel 300 to 700 series PCH layout).

### Graphics

After this kind of sleep the GPU kept its power and its state, while macOS graphics drivers expect the
cold GPU of an S3 resume. On the tested machine (Iris Xe with a patched framebuffer) the graphics kext
needed changes to wake cleanly. Things to try if the display does not come back:

- Graphics kexts can follow S0ixSleep: it publishes the `S0ixSleep` resource (an `OSData` holding the address
  of a small shared structure: enabled, "sleeping" from `kIOMessageSystemWillSleep` to
  `kIOMessageSystemHasPoweredOn`, sleep counter, last wake reason), see `S0ixSleep/kern_s0ix.hpp`.
- From version 2 the structure has `addHook()`: a kext adds a function that S0ixSleep calls inside the sleep,
  on the waiting CPU with interrupts off, once when entering the wait and once when leaving it. Register
  writes only (no locks, allocations or IOKit calls). The WhateverXe fork uses it to reset the GT (render
  engine) on the way out, as a power cycle would have.
- `GT_RESET=1` (Intel Gen12, Tiger Lake and newer): the same reset done by S0ixSleep itself, for graphics
  kexts that do not use the hook.

## Measuring

`sudo tools/sleep-power.sh 1800` puts the machine to sleep for 30 minutes (lid open, on battery), wakes it
with the safety timer and prints the whole-machine sleep power. Battery gauges that move in 1 % steps are
read at the moment of a step before and after the sleep; the awake part in between is integrated from the
sampled current (about ±0.25 W for 30 minutes). Compare settings with runs of the same length.

## Building

```
git clone https://github.com/acidanthera/MacKernelSDK
# Lilu headers: Lilu.kext from a Lilu DEBUG or RELEASE zip, in the repository root
python3 tools/build.py --release --output build/RELEASE
```

## Boot arguments

| | |
|---|---|
| `-s0ixoff` | do not load |
| `-s0ixdbg` | debug log (DEBUG build) |
| `s0ixecign=`, `s0ixecwake=` | EC event lists at boot |

## Credits

- [Lilu](https://github.com/acidanthera/Lilu) and [MacKernelSDK](https://github.com/acidanthera/MacKernelSDK) by acidanthera.
- The Linux kernel's s2idle and LPS0 code (`drivers/acpi/x86/s2idle.c`) and i915 documentation, for how
  Modern Standby firmware expects to be driven.

## License

BSD 3-Clause, see [LICENSE](LICENSE).
