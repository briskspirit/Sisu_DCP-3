# Sisu DCP-3 Firmware

[![License: Unlicense](https://img.shields.io/badge/License-Unlicense-yellow.svg)](https://unlicense.org/)

Sisu DCP-3 is a from-scratch C firmware project for a replacement Nokia 3210
mainboard. It supports the Rev B2 hardware:

- RP2354B with 2 MiB stacked flash
- Telit LE910C1-WWX cellular module
- NAU88C22 audio codec
- Nokia 3210 display, keypad, enclosure, and two-cell NiMH battery

The firmware recreates the Nokia 3210 NSE-8 v6.00 user experience from scratch.
Reverse engineering of the original firmware informs its behavior and
presentation. Net Monitor and the hardware service diagnostics are Sisu
engineering tools rather than features of the original phone.

## Build

The project uses CMake and the qualified Raspberry Pi Pico SDK 2.2.0 commit
`a1438dff1d38bd9c65dbd693f0e5db4b9ae91779`. A local SDK checkout in
`.pico-sdk/pico_sdk-src` is selected automatically; an explicit
`PICO_SDK_PATH` overrides the path but must identify that same clean commit.

```sh
cmake -S . -B build
cmake --build build
```

The default service image is `build/sisu_dcp3_revb2_telit.uf2` and retains USB
CDC diagnostics. A distributable image is built separately with
`-DRELEASE=ON`; it is named
`build-release/sisu_dcp3_revb2_telit_release.uf2` and contains neither CDC nor
the privileged debug console. Configuring with `-DSISU_BUILD_DIAGNOSTICS=ON`
additionally builds the standalone Rev B2 bench diagnostics and antenna tuner.
See [BUILD.md](BUILD.md) for the exact commands and BOOTSEL recovery gesture.

## Test

Run the host suite from any directory:

```sh
tests/run_tests.sh
```

The suite compiles pure firmware modules with `-Wall -Wextra`,
AddressSanitizer, and UndefinedBehaviorSanitizer. It also enforces the fixed
Rev B2/Telit product boundary, GPIO ownership, modem-vendor isolation, storage
layout, and call-model dependency rules. A fresh public checkout uses small
temporary synthetic assets automatically; when locally generated v6.00 assets
exist, the same command exercises those instead. See [BUILD.md](BUILD.md) for
the release-fidelity gate and host prerequisites.

## Architecture

- Core 0 owns application state, rendering, LCD updates, keypad input, storage,
  RTC, shared I2C, USB diagnostics, and the modem AT service.
- Core 1 owns real-time audio playback and the bidirectional 16 kHz voice bridge
  between the Telit DVI clock domain and the NAU88C22 clock domain.
- The Telit backend owns module commands, response grammar, power sequencing,
  provisioning, registration details, DTR/CTS sleep, RI wake, DVI setup, and
  dynamic antenna tuning.
- The generic modem service owns request scheduling, SMS and phonebook flows,
  call transactions, and the id-authoritative CLCC call table.
- Hardware access stays behind HAL and service ownership boundaries. Apps do
  not drive board power, modem control, or shared interrupt pins directly.

The Nokia-derived assets (fonts, bitmaps, strings, tones, T9 dictionaries)
are never committed: `src/generated/` and `include/generated/` are generated
at build time from a user-supplied, hash-verified copy of the original v6.00
firmware dropped into `firmware/` (see `firmware/README.md`). A fresh checkout
needs that firmware once; after that, builds work without it.

## Hardware Behavior

The shared 3.8 V rail powers both the Telit module and the magnetic buzzer.
`shared_3v8_service` arbitrates those users, while modem startup still
requires the separate Telit `ON_OFF_N` sequence. Ringtones, alarm tones,
warnings, game sounds, and SMS alert melodies use the buzzer. Key clicks, local
DTMF, soft notifications, and call audio use the codec path.

The powered-on standby controller uses peripheral idling, a 6 MHz quiet clock,
and POWMAN clock-dormant intervals while preserving keypad, charger, RTC,
headset, modem RI, call, and SMS wake behavior. The CDC-enabled service image
also wakes for USB; the release image deliberately does not. Power-off
progresses from a functional soft-off state into POWMAN P1.7 after all fallible
shutdown work and wake-source checks complete.

Settings and phone-local records use CRC-protected atomic records on separate
64 KiB system and 256 KiB user littlefs partitions. The old journal's space is
reclaimed after migration. Phonebook entries and SMS bodies remain modem-backed and are read
lazily. See [Storage engine](docs/storage_engine.md) for the flash layout.

## License

Project-authored code and documentation are released into the public domain
under the [Unlicense](COPYING). Separately licensed third-party files retain
their own terms; see [NOTICE.md](NOTICE.md). Contribution terms are in
[CONTRIBUTING.md](CONTRIBUTING.md).

Nokia is a trademark of Nokia Corporation. This is an unaffiliated hobby
recreation of a 1999 handset's user experience; it is not endorsed by or
associated with Nokia.

## Documentation

- [Build instructions](BUILD.md)
- [Documentation index](docs/README.md)
- [Rev B2 hardware contract](docs/revb2_hardware_contract.md)
