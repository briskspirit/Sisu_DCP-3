# Notices

Project-authored Sisu DCP-3 code and documentation are released into the public
domain under the [Unlicense](COPYING). Contribution terms are in
[CONTRIBUTING.md](CONTRIBUTING.md).

## Third-party components

- `src/audio/audio_i2s.pio` — derived from the MicroPython RP2 `machine.I2S`
  implementation. MIT License, Copyright (c) 2021 Mike Teachman. The MIT
  notice is retained in the file header and applies to that file.
- `cmake/pico_sdk_import.cmake` — derived from the Raspberry Pi Pico SDK's
  `pico_sdk_import.cmake`. BSD-3-Clause, Copyright (c) 2020 Raspberry Pi
  (Trading) Ltd. See the file header.
- `boards/sisu_sse_revb2.h` — follows the Pico SDK board-header convention
  and is licensed BSD-3-Clause (see the file header) so it can be reused
  with the SDK directly.
- Raspberry Pi Pico SDK — fetched at build time, not redistributed in this
  repository. BSD-3-Clause, Copyright (c) 2020 Raspberry Pi (Trading) Ltd.

Files carrying their own license header remain under that license. Other
project-authored material is released under the Unlicense.

## Trademarks

"Nokia" is a trademark of Nokia Corporation. "Telit" is a trademark of Telit
Cinterion. This is an independent hobby project recreating the user experience
of a 1999 handset on new hardware; it is not affiliated with, endorsed by, or
sponsored by Nokia or any other trademark holder named here.
