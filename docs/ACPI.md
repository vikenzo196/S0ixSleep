# ACPI: telling the firmware about sleep

S0ixSleep works without ACPI changes: the laptop sleeps and wakes. But on a Modern Standby laptop the
embedded controller (EC) only learns about sleep the Windows way, so without this part the fans may keep
spinning, the power LED stays on and the keyboard backlight or USB charging may not change.

## What Windows does

Windows does not call `_PTS`/`_WAK` for Modern Standby. It calls the `_DSM` of the PEP device
(`\_SB.PEPD`, `_HID` `INT33A1` or `_CID` `PNP0D80`) with two GUIDs:

| GUID | Function | Meaning |
|---|---|---|
| Intel LPS0 `c4eb40a0-6cd2-11e2-bcfd-0800200c9a66` | 3 / 4 | display off / on |
| | 5 / 6 | low power S0 entry / exit |
| Microsoft `11e00d56-ce64-47ce-837b-1f898f9aa461` | 3 / 4 | display off / on |
| | 7 / 8 | Modern Standby entry / exit |

Find `PEPD` (or the device with `_HID` `INT33A1`) in your DSDT and read what each function does. Often it
writes one or two EC fields (for example `MSST = One` in function 7), sometimes more.

macOS instead asks the firmware for S3: it runs `_PTS(3)` before sleep and `_WAK(3)` after wake.

## Option A: call the `_DSM` from `_PTS`/`_WAK`

[`acpi/SSDT-S0IX-TEMPLATE.dsl`](../acpi/SSDT-S0IX-TEMPLATE.dsl) calls the `_DSM` functions from
`_PTS(3)` and `_WAK(3)` when the OS is Darwin, in the order Linux uses. It needs two OpenCore ACPI
patches (ACPI → Patch) that rename the firmware's methods:

| Comment | Find | Replace |
|---|---|---|
| `_PTS to ZPTS` | `5F505453 01` | `5A505453 01` |
| `_WAK to ZWAK` | `5F57414B 01` | `5A57414B 01` |

The last byte is the method's flags: `01` for `Method (_PTS, 1, NotSerialized)`, `09` for a `Serialized`
method; use the one your DSDT has. Check that each pattern appears exactly once, and that no other SSDT
in your EFI already renames `_PTS`/`_WAK`; if one does, merge the calls into it.

This is untested on real hardware so far. Some `_DSM` implementations do much more than notify the EC
(USB, GPIO, dGPU, waiting for the display to be off) and may misbehave when called this way.

## Option B: write only the EC fields

Read what the `_DSM` functions write and replay only the EC writes from `_PTS(3)`/`_WAK(3)`. This is what
the tested machine uses:
[`acpi/examples/samsung-galaxy-book3/SSDT-S2QPTS.dsl`](../acpi/examples/samsung-galaxy-book3/SSDT-S2QPTS.dsl).

## Firmware S3 code in `_PTS`

`_PTS(3)` may contain code that only makes sense when the machine really powers down. On Samsung Galaxy
Books it sends the EC a "sleep is coming" command (`0x9F`/`0xA1`): the EC then blinks the power LED and
stops the keyboard until the next real power cycle, which an s0ix sleep never produces. If the keyboard
is dead or the LED blinks after wake, skip the firmware's `_PTS` for S3 on Darwin (template: `SKFP = One`)
and check what it does.

## Wake sources

Only GPEs that macOS armed for wake can end the sleep. If opening the lid does not wake the laptop, check
that the lid device (`PNP0C0D`) has a `_PRW` and that macOS shows it as a wake source
(`pmset -g log | grep -i wake`).

## Lid state after a timer wake

Firmware written for S3 often assumes that a wake from S3 means the lid was opened: its `_WAK` sets the
variable `_LID` returns to "open" and notifies the lid device. With S0ixSleep the machine also wakes from its
safety timer (hourly dark wakes), with the lid still closed. macOS then turns the display on and believes the
lid is open, and the next real lid opening may not wake the machine. Look for something like `LIDS = One`
(or `Local0 = One` stored into the lid variable) in the methods `_WAK` calls. The fix is to run that `_WAK`
code with the lid variable already "open" (no change, no notify), put the old value back, and publish the lid
state the embedded controller really reports, only if it changed: see the Samsung example.
