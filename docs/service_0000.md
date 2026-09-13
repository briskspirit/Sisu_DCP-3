# `*#0000#` — service / version screen

On the original **Nokia 3210 (NSE-8) v6.00**, entering `*#0000#` at standby shows
the software-version service record: three short lines (software version,
firmware date, model).

## Original 3210 v6.00 values (preserved here)

These are the exact strings the clone displayed before this screen was repurposed
to carry the Sisu build stamp:

| Line | Value       | Meaning                                             |
|------|-------------|-----------------------------------------------------|
| 1    | `V 06.00`   | Software version 6.00                               |
| 2    | `03-10-00`  | Firmware date, 3210 `DD-MM-YY` = 3 October 2000     |
| 3    | `NSE-8/9`   | Transceiver type (NSE-8 = 3210; /9 = the 3210 variant) |

Source: `src/apps/service_codes_app.c`, `handle_standby_service_code()`, the
`*#0000` branch.

## What the screen shows now (Sisu DCP-3)

A **sanctioned deviation** from 1:1 (like Net Monitor): because this is our own
firmware, `*#0000#` now reports the Sisu build identity instead of the frozen
3210 version, so a flashed board can be identified at a glance:

| Line | Value                | Source                                               |
|------|----------------------|------------------------------------------------------|
| 1    | short git commit hash | `SISU_BUILD_HASH` — generated at build time          |
| 2    | build date `YYYY-MM-DD` | `SISU_BUILD_DATE` — generated at build time        |
| 3    | build-target board    | `SISU_HW_REV_NAME` (a compile definition in `CMakeLists.txt`), currently `RevB2` |

Lines 1–2 are stamped into `generated/build_info.h` on every build by
`cmake/gen_build_info.cmake` (an always-run `sisu_build_info` target; the header
is only rewritten when the values change, so it doesn't force a rebuild). Line 3
is a compile-time constant — bump `SISU_HW_REV_NAME` in `CMakeLists.txt` with the PCB rev.

The other service codes (`*#06#`, `*#92702689#`, `*3370#`, etc.) are unchanged.
