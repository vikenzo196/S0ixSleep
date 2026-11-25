# Samsung Galaxy Book3 Ultra (NP964XFH)

The tested machine. Galaxy Book2/Book3 models share most of this firmware; check your own DSDT.

- `SSDT-S2QPTS.dsl` (version 3) replaces the firmware's `QPTS`/`QWAK` (called from `_PTS`/`_WAK`) for macOS S3:
  - **Sleep:** skips the firmware's `QPTS`, whose EC command `0x9F`/`0xA1` leaves the keyboard dead after an
    s0ix wake, and sets the EC's Modern Standby fields `CSMF`/`MSST` the way the PEPD `_DSM` does (fans and
    power LED off while asleep).
  - **Wake:** clears them again and runs the firmware's `QWAK`, but keeps the lid state honest. The firmware's
    `QWAK` assumes that any wake from S3 means the lid was opened: it sets `LIDS` (what `_LID` returns) to open and
    notifies `LID0`. After a timer (dark) wake with the lid still closed, macOS then turns the display on and
    believes the lid is open, which also stops the next real lid opening from waking the machine. Version 3
    makes `QWAK` see no change and then publishes the state the EC really reports (`LSTE`), only if it changed.
  - Needs two DSDT renames: method `\_SB.QPTS` to `XPTS` (`14 40 07 51505453 01` → `14 40 07 58505453 01`) and
    `\_SB.QWAK` to `XWAK` (`14 43 1B 5157414B 01` → `14 43 1B 5857414B 01`).
- `s0ixsleep.conf`:

  ```
  # _Q51-_Q54 AC/battery, _Q57/_Q58/_Q5B, _Q60/_Q61/_Q65/_Q66 battery, _Q72-_Q76 thermal/fan/UCSI, _Q7C SCAI
  EC_IGNORE=51,52,53,54,57,58,5b,60,61,65,66,72,73,74,75,76,7c
  ```

  Power button `_Q50` and lid `_Q5E`/`_Q5F` still wake.
- Graphics: WhateverXe fork (Iris Xe, OLED with DSC) with `-wex-s0ixsleep`; it resets the GT from an S0ixSleep
  wait hook and keeps the panel dark through the sleep.
