# Build

## Original-firmware assets

Generated asset sources (UI strings, fonts, bitmaps, tones, T9) derive from
the original Nokia 3210 v6.00 firmware and are NOT in the repository — they
embed Nokia/Tegic-derived data. Drop the firmware into `firmware/` (hashes
and sources in [firmware/README.md](firmware/README.md)) and every build
regenerates the assets from it — a stamp skips the ~20 s pipeline when
nothing changed. A fresh checkout therefore needs the firmware once; after
that, builds work without it from the previously generated files, and the
build fails with instructions only when neither is present.
`SISU_ASSET_LANGS` / `SISU_T9_LANGS` select the compiled language and
dictionary sets (English is always included).

This firmware is built with the Raspberry Pi Pico SDK and CMake. The sole
production target is the Rev B2 RP2354B board with a Telit LE910C1 modem. Pico
SDK metadata lives in `boards/sisu_sse_revb2.h`; the application pin map and
electrical contract live in `include/hal/board.h`.
See `docs/revb2_hardware_contract.md` for the complete production hardware
contract and bench-qualified subsystem inventory.

## Requirements

- CMake
- Git
- Python 3
- ARM embedded toolchain compatible with Pico SDK builds
- Raspberry Pi Pico SDK 2.2.0 at qualified commit
  `a1438dff1d38bd9c65dbd693f0e5db4b9ae91779`

No SDK setup is required. The first configure fetches the exact qualified SDK
commit into a local `.pico-sdk/` cache (gitignored); later configures select and
verify that checkout automatically. A mismatched revision or dirty SDK worktree
fails the configure instead of silently changing the firmware toolchain. The
default build is the CDC-enabled service firmware used for development and
bench work:

```sh
cmake -S . -B build
cmake --build build
```

An intentional SDK experiment can bypass qualification with
`-DSISU_ALLOW_UNQUALIFIED_PICO_SDK=ON`; CMake emits a warning and the result is
not a release-qualified build.

To use an SDK checkout you already have instead of the fetched cache, point
CMake at it (an explicit `PICO_SDK_PATH` always wins):

```sh
cmake -S . -B build -DPICO_SDK_PATH=/path/to/pico-sdk
cmake --build build
```

Its outputs are `sisu_dcp3_revb2_telit.uf2`,
`sisu_dcp3_revb2_telit.elf`, and related Pico SDK artifacts under `build/`.

For a distributable firmware without RP USB CDC or the privileged service
console, configure a separate build directory with the explicit `RELEASE`
flag:

```sh
cmake -S . -B build-release -DRELEASE=ON
cmake --build build-release
```

The release outputs are `sisu_dcp3_revb2_telit_release.uf2` and
`sisu_dcp3_revb2_telit_release.elf` under `build-release/`. This profile does
not attach TinyUSB, does not treat service VBUS as a clock/sleep blocker, and
does not wake from GP28 in powered standby or P1.7. Recovery remains available:
connect USB first, then hold Power for 5 seconds to enter the RP boot ROM's
BOOTSEL mode. Releasing after the normal 1.2-second hold still powers on the
phone normally.

Standalone Rev B2 diagnostics and the antenna tuner are opt-in: configure with
`-DSISU_BUILD_DIAGNOSTICS=ON` to build them alongside the firmware. There are
no hardware-revision or modem-vendor build selectors.

The opt-in [storage power-cut bench](docs/storage_powercut_test.md) instead
replaces the phone runtime for offline battery-removal testing of the historical
128 KiB layout. Do not use it after the system/user partition split. Its separate
`SISU_STORAGE_POWERCUT_BENCH` option must remain off in ordinary builds.

## Host tests

The host suite requires Bash, Python 3, ripgrep, ordinary POSIX text tools, and
a Clang or GCC C compiler/linker with AddressSanitizer and
UndefinedBehaviorSanitizer. It probes the linker and selects Darwin
`-dead_strip` or ELF `--gc-sections` as appropriate.

```sh
tests/run_tests.sh
```

The command is self-contained on a fresh public checkout. If private generated
assets are absent, it creates deterministic, non-Nokia fixtures under its
temporary test directory and removes them afterward. When real generated
assets exist, they are used automatically. Release/fidelity verification can
require those real assets explicitly:

```sh
SISU_TEST_REQUIRE_ORIGINAL_ASSETS=1 tests/run_tests.sh
```

CI deliberately sets `SISU_TEST_FORCE_SYNTHETIC_ASSETS=1` on both Linux and
macOS so the public path and both linker variants remain continuously tested.
