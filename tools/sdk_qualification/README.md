# Pico SDK Hardware Tests

Standalone Rev B2 test images for SDK upgrades. They do not run the phone
application, write its storage, or enable the modem rail. Turn the modem off
cleanly before flashing, back up the DUT, and keep a normal service UF2 ready
to restore afterward. Do not flash them during a battery experiment or call.

From the repository root, using a clean checkout of the pinned SDK:

```sh
cmake -S tools/sdk_qualification -B build-sdk-tests \
  -DPICO_SDK_PATH=/absolute/path/to/pico-sdk \
  -DCMAKE_BUILD_TYPE=Release
cmake --build build-sdk-tests -j8
```

The SDK's `lib/tinyusb` submodule must be initialized. No original Nokia assets
are needed. An intentional alternate-SDK experiment requires the same
`SISU_ALLOW_UNQUALIFIED_PICO_SDK=ON` override as the phone build; the POWMAN
test uses APIs available in SDK 2.3.1.

The upstream tests are compiled unchanged:

| Build-directory path | Coverage |
|---|---|
| `short_sleep_test/short_sleep_test.uf2` | Short sleeps, hardware spinlocks |
| `short_sleep_test/short_sleep_test_sw.uf2` | Short sleeps, software spinlocks |
| `pico_sync_test/pico_sync_test.uf2` | Synchronization and low-power wait loops |
| `pico_sync_test/pico_sync_test_sw.uf2` | Same, software spinlocks |
| `pico_time_test/pico_time_test.uf2` | Alarm ordering, cancellation and races |
| `pico_time_test/pico_time_test_sw.uf2` | Same, software spinlocks |
| `powman_handoff_test.uf2` | Counter continuity across 400 XOSC/LPOSC cycles |

Flash one image at a time with `picotool load -v -x <image> -f --ser <serial>`.
After the test finishes, open its CDC port at 115200 baud. Sending `r` repeats
the result. A pass requires `END result=0`, `timeout=0`, `overflow=0`, and no
failed sections. Keep the transcript outside tracked source files. Restore
the normal service UF2 when finished, including after a test failure.

The runner applies the production safe-GPIO configuration and leaves UART
stdio disabled because UART0 is connected to the modem. Test output is captured
in RAM; USB starts only after the test, so its interrupts cannot change timing
results. The initial system clock is 153.6 MHz; upstream tests may change it.
An eight-second watchdog, fed by test progress output, reports a hang after
reboot instead of rerunning the failing test indefinitely. A failure before
the runner or USB initialization can still require physical BOOTSEL recovery.

The POWMAN test covers changed/unchanged calibration branches, low-word counter
rollover, and both SDK-only and explicit-checkpoint handoffs. It leaves the CPU
clock running. It does **not** validate clock-dormant entry, peripheral clock
restoration, real GPIO wake, or P1.7; those remain phone-level bench checks.
