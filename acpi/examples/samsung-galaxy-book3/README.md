# Samsung Galaxy Book3 Ultra (NP964XFH)

The tested machine. Galaxy Book2/Book3 models share most of this firmware; check your own DSDT.

- `SSDT-S2QPTS.dsl` (version 4) replaces the firmware's `QPTS`/`QWAK` (called from `_PTS`/`_WAK`) for macOS S3,
  and wraps `GPTS` for shutdown:
  - **Sleep:** skips the firmware's `QPTS`, whose EC command `0x9F`/`0xA1` leaves the keyboard dead after an
    s0ix wake, and sets the EC's Modern Standby fields `CSMF`/`MSST` the way the PEPD `_DSM` does (fans and
    power LED off while asleep).
  - **Wake:** clears them again and runs the firmware's `QWAK`, but keeps the lid state honest. The firmware's
    `QWAK` assumes that any wake from S3 means the lid was opened: it sets `LIDS` (what `_LID` returns) to open and
    notifies `LID0`. After a timer (dark) wake with the lid still closed, macOS then turns the display on and
    believes the lid is open, which also stops the next real lid opening from waking the machine. Version 3
    makes `QWAK` see no change and then publishes the state the EC really reports (`LSTE`), only if it changed.
  - **Shutdown:** lets opening the lid power the laptop on, as after a Windows shutdown (see below).
  - Needs three DSDT renames: method `\_SB.QPTS` to `XPTS` (`14 40 07 51505453 01` → `14 40 07 58505453 01`),
    `\_SB.QWAK` to `XWAK` (`14 43 1B 5157414B 01` → `14 43 1B 5857414B 01`) and `\_SB.GPTS` to `XGPT`
    (`14 42 05 47505453 01` → `14 42 05 58475054 01`). Check that each pattern appears once in your DSDT.
- `s0ixsleep.conf`:

  ```
  # _Q51-_Q54 AC/battery, _Q57/_Q58/_Q5B, _Q60/_Q61/_Q65/_Q66 battery, _Q72-_Q76 thermal/fan/UCSI, _Q7C SCAI
  EC_IGNORE=51,52,53,54,57,58,5b,60,61,65,66,72,73,74,75,76,7c
  ```

  Power button `_Q50` and lid `_Q5E`/`_Q5F` still wake.
- Graphics: WhateverXe fork (Iris Xe, OLED with DSC) with `-wex-s0ixsleep`; it resets the GT from an S0ixSleep
  wait hook and keeps the panel dark through the sleep.

## Lid

The lid's EC event is `_Q5F`, which copies `LSTE` (EC RAM byte 0x83, bit 0) into `LIDS`. After some sleeps the
EC does not send `_Q5F` when the lid opens, so also set `LID_EC=83.0` in `s0ixsleep.conf`: S0ixSleep then reads
`LSTE` during the sleep and wakes when the lid opens. (`_L12`, GPE 0x12, also handles the lid in the firmware, but
that GPE does not fire under macOS.)

## Power on by opening the lid

After a Windows shutdown, opening the lid powers the laptop on; after a macOS shutdown it did not. Before S5,
`\_SB.PTS` calls `GPTS(5)`, which passes the SMI handler `0x05` when the Samsung Windows driver has set
`\_SB.SCAI.DVLD` (through `SCAI.SDLS`), and `0x15` otherwise. Version 4 of the SSDT sets `DVLD` on macOS for S5 only,
then calls the firmware's `GPTS` (renamed `XGPT`): the lid now powers the laptop on after a macOS shutdown too.
`DVLD` is not set at boot: it also routes the EC's hotkey events to `SCAI`, which macOS does not handle.
