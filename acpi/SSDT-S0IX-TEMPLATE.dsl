/*
 * S0ixSleep: template SSDT that tells the firmware about Modern Standby entry and exit.
 *
 * UNTESTED TEMPLATE. Read docs/ACPI.md first and adapt it to your DSDT.
 *
 * macOS asks the firmware for S3, so it runs _PTS(3) before sleep and _WAK(3) after wake. On a
 * Modern Standby laptop the embedded controller (fans, power LED, keyboard, USB charging) is told
 * about sleep a different way: Windows and Linux call the _DSM of the PEP device (\_SB.PEPD, _HID
 * INT33A1 / PNP0D80) with the Microsoft Modern Standby GUID and, on Intel, the LPS0 GUID. This SSDT
 * makes those calls from _PTS(3) and _WAK(3), only when the OS is Darwin, in the order Linux uses
 * (drivers/acpi/x86/s2idle.c):
 *   sleep: Intel display off (3), MS display off (3), MS sleep entry (7), Intel LPS0 entry (5)
 *   wake:  Intel LPS0 exit (6), MS sleep exit (8), MS display on (4), Intel display on (4)
 *
 * Requires OpenCore ACPI patches that rename the firmware's _PTS to ZPTS and _WAK to ZWAK
 * (see docs/ACPI.md), so that this SSDT's _PTS/_WAK run first and call them.
 *
 * Firmware S3 code in _PTS may assume the machine really powers off (on Samsung Galaxy Books it sends
 * the EC a "sleep is coming" command that leaves the keyboard dead after an s0ix wake). If your wake
 * has a dead keyboard or a blinking LED, skip the firmware's _PTS for S3 (SKIP_FW_PTS below) and check
 * what it does.
 */
DefinitionBlock ("", "SSDT", 2, "S0IX", "S0IXDSM", 0x00001000)
{
    External (\_SB.PEPD, DeviceObj)
    External (\_SB.PEPD._DSM, MethodObj)
    External (\ZPTS, MethodObj)
    External (\ZWAK, MethodObj)

    // 1 = do not run the firmware's _PTS for S3 on macOS (see the header). 0 = run it.
    Name (SKFP, Zero)

    Method (S0XD, 3, Serialized)
    {
        // Arg0: GUID, Arg1: revision, Arg2: function
        If (CondRefOf (\_SB.PEPD._DSM))
        {
            \_SB.PEPD._DSM (Arg0, Arg1, Arg2, Package (Zero) {})
        }
    }

    Method (_PTS, 1, NotSerialized)
    {
        If (_OSI ("Darwin") && (Arg0 == 0x03))
        {
            Local0 = ToUUID ("c4eb40a0-6cd2-11e2-bcfd-0800200c9a66")   // Intel LPS0
            Local1 = ToUUID ("11e00d56-ce64-47ce-837b-1f898f9aa461")   // Microsoft Modern Standby
            S0XD (Local0, One, 0x03)
            S0XD (Local1, Zero, 0x03)
            S0XD (Local1, Zero, 0x07)
            S0XD (Local0, One, 0x05)
            If (SKFP)
            {
                Return (Zero)
            }
        }

        ZPTS (Arg0)
        Return (Zero)
    }

    Method (_WAK, 1, NotSerialized)
    {
        If (_OSI ("Darwin") && (Arg0 == 0x03))
        {
            Local0 = ToUUID ("c4eb40a0-6cd2-11e2-bcfd-0800200c9a66")
            Local1 = ToUUID ("11e00d56-ce64-47ce-837b-1f898f9aa461")
            S0XD (Local0, One, 0x06)
            S0XD (Local1, Zero, 0x08)
            S0XD (Local1, Zero, 0x04)
            S0XD (Local0, One, 0x04)
        }

        Return (ZWAK (Arg0))
    }
}
