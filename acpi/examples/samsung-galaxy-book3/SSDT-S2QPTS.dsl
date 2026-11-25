/*
 * macOS s2idle-style sleep on the Galaxy Book3 Ultra (no working S3).
 *
 * Sleep (_PTS -> \_SB.PTS -> QPTS): the firmware's QPTS sends the EC mailbox
 * command 0x9F/0xA1 ("sleep is coming"): the EC then blinks the power LED and
 * stops the keyboard until SLP_S3#/PLTRST# reset it, which a sleep without S3
 * never produces, and _WAK has no command to undo it. On macOS, for S3 only,
 * skip it and instead tell the EC what Windows Modern Standby tells it through
 * the PEPD _DSM (this firmware only supports S0ix):
 *   Intel  c4eb40a0 fn3 (display off)  -> H_EC.CSMF = 1
 *   MS     11e00d56 fn7 (sleep entry)  -> H_EC.MSST = 1
 *   Intel  c4eb40a0 fn5 (LPS0 entry)   -> H_EC.CSMF = 3
 * Wake (_WAK -> \_SB.WAK -> QWAK), reverse order:
 *   Intel fn6 (LPS0 exit) CSMF = 1, MS fn8 (sleep exit) MSST = 0,
 *   Intel fn4 (display on) CSMF = 0, then the firmware's QWAK.
 * Only the EC flags are replayed, not the rest of those _DSM functions (GUAM,
 * dGPU, XHCI, GPIO hooks, display-off wait loop).
 *
 * Lid (v3): the firmware's QWAK assumes that a wake from S3 means the lid was
 * opened: it sets LIDS (what _LID returns) to open and notifies LID0. An s2idle
 * wake can also be a timer (dark) wake with the lid still closed: macOS then
 * turns the display on and believes the lid is open. On macOS S3 the firmware
 * QWAK sees LIDS already "open" (no change, no notify), LIDS is put back, and
 * the lid state the EC really reports (LSTE) is published only if it changed.
 *
 * Power on by opening the lid (v4): before a shutdown (_PTS(5)) the firmware's
 * GPTS hands the SMI handler 0x05 when the Samsung Windows driver is loaded
 * (\_SB.SCAI.DVLD, set by its SDLS call) and 0x15 otherwise; only after the
 * former does opening the lid power the laptop on. On macOS, for S5 only, set
 * DVLD before calling the firmware's GPTS (renamed XGPT).
 *
 * Requires in DSDT.aml the method definitions \_SB.QPTS -> \_SB.XPTS
 * (14 40 07 "QPTS" 01 -> 14 40 07 "XPTS" 01) and \_SB.QWAK -> \_SB.XWAK
 * (14 43 1B "QWAK" 01 -> 14 43 1B "XWAK" 01), and \_SB.GPTS -> \_SB.XGPT
 * (v4). S4/S5 keep the original path, apart from DVLD before S5.
 */
DefinitionBlock ("", "SSDT", 2, "VIK", "S2QPTS", 0x00004000)
{
    External (_SB_.XPTS, MethodObj)
    External (_SB_.XWAK, MethodObj)
    External (_SB_.XGPT, MethodObj)
    External (_SB_.SCAI.DVLD, IntObj)
    External (_SB_.PC00.LPCB.H_EC.CSMF, FieldUnitObj)
    External (_SB_.PC00.LPCB.H_EC.MSST, FieldUnitObj)
    External (_SB_.PC00.LPCB.H_EC.LSTE, FieldUnitObj)
    External (_SB_.PC00.GFX0.CLID, FieldUnitObj)
    External (_SB_.LID0, DeviceObj)
    External (ECON, FieldUnitObj)
    External (LIDS, FieldUnitObj)
    External (IGDS, FieldUnitObj)

    Scope (_SB)
    {
        Method (GPTS, 1, NotSerialized)
        {
            If (_OSI ("Darwin"))
            {
                If ((Arg0 == 0x05))
                {
                    \_SB.SCAI.DVLD = One
                }
            }

            XGPT (Arg0)
        }

        Method (QPTS, 1, NotSerialized)
        {
            If (_OSI ("Darwin"))
            {
                If ((Arg0 == 0x03))
                {
                    If (ECON)
                    {
                        \_SB.PC00.LPCB.H_EC.CSMF = One
                        \_SB.PC00.LPCB.H_EC.MSST = One
                        \_SB.PC00.LPCB.H_EC.CSMF = 0x03
                    }

                    Return (Zero)
                }
            }

            XPTS (Arg0)
            Return (Zero)
        }

        Method (QWAK, 1, NotSerialized)
        {
            If (_OSI ("Darwin"))
            {
                If ((Arg0 == 0x03))
                {
                    If (ECON)
                    {
                        \_SB.PC00.LPCB.H_EC.CSMF = One
                        \_SB.PC00.LPCB.H_EC.MSST = Zero
                        \_SB.PC00.LPCB.H_EC.CSMF = Zero
                    }

                    Local1 = LIDS
                    LIDS = One
                    XWAK (Arg0)
                    LIDS = Local1
                    If (ECON)
                    {
                        Local2 = \_SB.PC00.LPCB.H_EC.LSTE
                        If ((Local2 != LIDS))
                        {
                            LIDS = Local2
                            If (IGDS)
                            {
                                If ((LIDS == Zero))
                                {
                                    \_SB.PC00.GFX0.CLID = 0x80000000
                                }
                                Else
                                {
                                    \_SB.PC00.GFX0.CLID = 0x80000003
                                }
                            }

                            Notify (\_SB.LID0, 0x80) // Status Change
                        }
                    }

                    Return (Zero)
                }
            }

            XWAK (Arg0)
            Return (Zero)
        }
    }
}
