# Rev B2 Hardware Contract

Status: implemented and bench-qualified on the assembled Rev B2 prototype.

This is the maintained firmware-facing hardware contract for Sisu DCP-3. The
only production combination is:

- RP2354B, 2 MiB stacked flash, 12 MHz crystal
- Telit LE910C1-WWX cellular module
- Nokia 3210 NSE-8 mechanical and user-interface envelope

Compatibility board profiles and alternate cellular backends are outside the
project scope. Keep hardware variation behind HAL and service interfaces where
that boundary remains useful, but do not add dormant product selectors.

## Sources Of Truth

Use these sources in this order:

1. The manufactured Rev B2 schematic and PCB in the hardware project.
2. `boards/sisu_sse_revb2.h` for Pico SDK package and flash metadata.
3. `include/hal/board.h` for the firmware pin and polarity contract.
4. Bench-qualified behavior documented in focused subsystem notes.

Signal names are authoritative. Reference designators may change between PCB
layout iterations.

## Build Identity

The fixed production target is `sisu_dcp3_revb2_telit`. It defines:

```text
SISU_HW_REV_B2=1
SISU_MODEM_VENDOR_TELIT=1
SISU_MODEM_BACKEND_ENABLED=1
```

Standalone modem-free diagnostics use the same Rev B2 board definition with an
inert `NONE` backend. That is a test composition, not another product.

The RP2354B uses the RP2350B 48-GPIO package (`PICO_RP2350A=0`). GPIO 40-43 are
ADC0-ADC3. GPIO 26-29 are not ADC inputs on this package.

## GPIO Ledger

| GPIO | Direction | Function |
|---:|:---:|---|
| 0 | out | UART0 TX to Telit |
| 1 | in | UART0 RX from Telit |
| 2 | in | UART0 CTS from Telit |
| 3 | out | UART0 RTS to Telit |
| 4 | in | TPS63020 `+3V8` power good |
| 5 | out | Magnetic buzzer drive |
| 6 | out | TPS63020 `+3V8` enable, active high |
| 7 | in | Power button, active low |
| 8 | in | Telit DVI/I2S BCLK |
| 9 | in | Telit DVI/I2S word align |
| 10 | in | Telit DVI/I2S downlink data |
| 11 | out | Telit DVI/I2S uplink data |
| 12 | in | NAU88C22 ADCOUT |
| 13 | out | NAU88C22 LRCLK |
| 14 | out | NAU88C22 BCLK |
| 15 | out | NAU88C22 DACIN |
| 16 | out | LCD reset |
| 17 | out | LCD chip select |
| 18 | out | LCD SPI clock |
| 19 | out | LCD SPI data |
| 20 | out | LCD data/command |
| 21 | out | NAU88C22 MCLK |
| 22 | out | Vibra PWM |
| 23 | out | LCD backlight PWM |
| 24 | I/O | I2C0 SDA |
| 25 | out | I2C0 SCL |
| 26-27 | - | Unconnected |
| 28 | in | Service VBUS detect, active high digital input |
| 29-32 | - | Unconnected |
| 33 | out | TPS63020 force-PWM control |
| 34-35 | - | Unconnected |
| 36 | out | Telit DTR through level translator |
| 37 | in | Telit RI through level translator, active low |
| 38 | out | Telit `ON_OFF_N` gate; RP high asserts |
| 39 | out | Telit `HW_SHUTDOWN_N` gate; RP high asserts |
| 40/ADC0 | in | Telit PWRMON/VAUX monitor through divider |
| 41/ADC1 | in | Headset hook resistor-ladder ADC |
| 42 | in | Shared active-low system interrupt |
| 43/ADC3 | in | Nokia charger-input voltage detect |
| 44-47 | - | Unconnected |

Unused and powered-off pads must follow `board_safe_gpio` and the dormant pad
ledger. In particular GP6, GP33, GP38, and GP39 latch low; modem-facing inputs
are parked so an unpowered level translator cannot create input-buffer current.

## Shared 3.8 V Rail

The TPS63020 rail powers both the cellular module and magnetic buzzer.

- GP6 high enables the rail; low disables it.
- GP4 is read-only power good.
- GP33 low permits power-save operation; high forces PWM mode.
- `shared_3v8_service` is the sole production owner of rail policy.
- Modem and audio clients acquire independent ownership bits.
- Dormant entry requires no logical owner and a physically inactive rail.
- GP5 is muxed to PWM only while a buzzer waveform is active. Buzzer stop and
  initialization remux it to SIO at the inactive-low level; merely disabling an
  RP2350 PWM slice is insufficient because the pad can retain its last phase
  and apply damaging DC to the transducer.

No app or modem backend may drive these board pins directly. Diagnostics may do
so only in standalone targets with explicit user commands and safety bounds.

## Telit Power And UART

Normal startup is non-blocking:

1. Acquire the modem `+3V8` owner.
2. Require GP4 power good.
3. Pulse GP38 to assert `ON_OFF_N`.
4. Wait for qualified GP40 PWRMON evidence.
5. Require UART RX idle-high before enabling PL011 RX interrupts.
6. Establish hardware flow control, initialize, provision, then permit service.

Before the first READY state, one observed PWRMON drop with supply PG still
good is allowed to be a module-initiated restart, such as automatic carrier
selection after a SIM swap. Firmware retains the rail, parks UART, waits within
the startup budget for PWRMON and RX-idle readiness, and reruns full AT init and
provisioning. This does not consume the separate one-reboot provisioning budget.
A second unexpected drop fails rather than looping. Once READY has been reached,
an unexpected PWRMON drop still follows the runtime fault policy; a PG failure is
never treated as a carrier restart. An explicit OFF request survives the wait.

Normal shutdown uses `AT#SHDN`, parks PL011, and keeps the rail powered until
PWRMON is continuously below the qualified off threshold. A bounded failure
ladder then retries once through a 3 s GP38 graceful hardware-off pulse and,
only after its separate 60 s finalization budget expires, once through a 250 ms
GP39 unconditional pulse. A hardware alarm owns pulse deassertion. Every stage
still requires 500 ms qualified-low PWRMON before the rail can be released; a
terminal failure retains the rail and becomes visible to the user. The complete
policy and its Rev B2 evidence are in `docs/modem_shutdown_recovery.md`.

UART0 runs at 115200 baud with hardware RTS/CTS. DTR permits Telit sleep and CTS
is the transport-sleep evidence. RI wakes the RP for calls, SMS, and buffered
unsolicited results. UART writes are bounded so a CTS fault cannot deadlock the
UI core.

Incoming SMS use direct delivery on every carrier: init applies `AT+CSDH=1` and
`AT+CNMI=2,2,0,0,0`, the module hands each message over as a `+CMT` URC (the
standard 3GPP text/PDU forms, or the Telit 3GPP2 forms a Verizon SIM produces),
and the firmware rebuilds it as a 3GPP TS 23.040 SMS-DELIVER PDU and writes it
into the ME store with `AT+CMGW=<len>,0` before the ordinary mailbox scan sees
it. The resting SMS mode stays text (`AT+CMGF=1`); plain bodies are read raw by
`<length>` so line breaks, spaces and `@` survive. Store mode (`+CMTI`) is no
longer used because the Verizon image files 3GPP2 messages in a CDMA store
this backend cannot read (`$QCMTI`). Design, evidence and bench results:
`docs/sms_direct_delivery_design.md`.

Unsolicited RI-triggered DTR wake starts only in READY. Startup and controlled
reboots can toggle RI/CTS without being asleep, so their readiness is handled by
the startup waiter, not the short sleep-wake timeout. Explicit command wake-ups
during initialization and provisioning remain CTS-gated as before.

For UART diagnostics, `modemrx on` starts an opt-in 4096-byte RAM capture before
line parsing; `modemrx dump` stops it and prints the retained bytes as hex.
The capture is off at boot, overwrites its oldest bytes when full, and reports
the overwritten count. `modemrx off` stops without dumping. Starting a new
capture discards the previous snapshot. It does not change receive policy or
store SMS, but its output can contain message bodies and subscriber identifiers.

## Telit Provisioning

The Telit backend owns all module-specific commands and response grammar. The
generic modem service owns scheduling, transactions, status publication, and
recovery policy.

The required provisioning includes:

- command-mode hardware flow control
- SIM observation and readiness
- persistent SIM-based carrier selection (`#FWAUTOSIM=1`)
- 60-second no-coverage scan pause (`#NWSCANTMR=60`)
- LTE registration and signal indications
- SMS delivery and RI wake behavior
- `#ECAM` call events plus authoritative `+CLCC` reconciliation
- DVI/OAP voice transport
- disabled GNSS and RX diversity because those RF paths are not populated
- dynamic antenna GPIO alternate functions and `#STUNEANT` policy

Persistent settings use query/compare/write/verify and a bounded reboot budget.
Blind writes must not be added to the generic init path.

Carrier selection is checked before board provisioning and repaired only on a
valid mismatch. Firmware does not force an AT&T or Verizon profile with
`#FWSWITCH`. Automatic selection is not a guarantee of reboot-free SIM swaps:
a carrier change can restore module settings, which must be verified again.
If carrier-selection or scan-pause verification fails, the modem can remain
usable, but provisioning is reported as unverified.

## Dynamic Antenna Tuning

Telit GPIO2 and GPIO3 drive the four-state RF switch. Firmware programs the
bench-derived band masks through `#STUNEANT`; unsupported command-domain bands
fall back to the electrically deterministic RF1 state.

The maintained throw names are:

| Throw | Primary tuned use |
|---|---|
| RF1 | LTE B2 and other high-band assignments |
| RF2 | LTE B12/B14 and related low-band assignments |
| RF3 | LTE B5 and related 850/900 MHz assignments |
| RF4 | LTE B4/B66 and related AWS assignments |

The complete masks and GPIO truth table live beside their parser and static
assertions in `src/services/modem_vendor_telit_tune.c`. Manual RF-off tuning firmware
is isolated under `tools/tune_dynamic_ant/` and must never be composed into the
production target.

## Digital Voice And Codec

The Telit DVI side is the clock master on GP8-GP11. The RP captures downlink,
produces uplink, and bridges that clock domain to the NAU88C22 domain using two
lock-free elastic rings on core 1.

The codec clock/data layout is fixed:

```text
GP12 ADCOUT
GP13 LRCLK
GP14 BCLK
GP15 DACIN
GP21 MCLK
```

DMA IRQ0 belongs to the codec and DMA IRQ1 belongs to the modem DVI transport at
equal NVIC priority. PIO/DMA must be quiesced across flash writes and dormant
entry. Every voice session performs the qualified resync sequence that clears
PIO shift residue and rebuilds both DMA ping-pongs.

Button clicks, standby DTMF, and soft notifications use the earpiece path.
Ringtones, alarm, warning/game tones, and SMS alert melodies use the magnetic
buzzer and therefore acquire the shared `+3V8` audio owner.
The fitted CMT-1085-85-SMT is not loudest at 50% duty in this circuit: the
measured production ladder peaks at 24%. Calibration evidence and the remaining
stock Level-5 output gap are recorded in `tools/audio_calibration/README.md`.

## I2C And Shared Interrupt

I2C0 runs at 400 kHz and every transfer is bounded by
`BOARD_I2C_TIMEOUT_US`.

| Address | Device | Purpose |
|:---:|---|---|
| `0x1A` | NAU88C22 | Audio codec |
| `0x32` | RV-8803 | RTC and alarm |
| `0x34` | TCA8418 | Keypad, headset insert, charger control/status |
| `0x63` | LTC2959 | Battery voltage/current/coulomb monitor |

GP42 is a wired-OR active-low summary from TCA8418, RV-8803, and LTC2959.
`board_irq_hal` is the sole GPIO callback owner. The shared IRQ service polls
and clears each source; it must not infer which device asserted from the GPIO
level alone.

## Battery, Charger, And Accessories

The battery is a two-cell NiMH pack. LTC2959 is authoritative for battery
voltage and current. Firmware journals qualified full-to-empty
delivered-capacity history and effective-resistance diagnostics; the IC itself
does not learn pack capacity. Qualified health-supervisor SOC owns battery bars
and reports predicted capacity exhaustion, while fused physical voltage/supply
evidence owns EMPTY. Outside a call, exhausted anchored capacity plus the
debounced voltage LOW state is sufficient corroboration to commit EMPTY before
the loaded rail reaches its hard floor. Anchored charge left suppresses in-call
load sag as an endpoint; the 1800 mV emergency floor can still shut down without
teaching a false capacity sample. Startup admission remains voltage-owned.

Nokia charger state combines TCA8418 charger status/control pins with GP43
analog input presence. Service VBUS on GP28 is unrelated to Nokia charging and
must not enter charger policy.

BQ25171 `/CE` is active-low on TCA8418 COL5. Rev B2 R77, the intended 10 kOhm
fail-enabled pull-down, is DNP in the design source. Firmware drives and verifies
COL5 low, but an unreworked, deeply depleted board may not execute firmware soon
enough to enable charging. The bench DUT has its 10 kOhm R77 rework; every Rev
B2 assembly requires the same fit, and the next PCB must populate it by default.
The complete failure analysis is in
`docs/revb2_charger_enable_erratum.md`.

The bench-reworked charger uses 1.10 kOhm on `ISET` for approximately 273 mA
total output and 24 kOhm on `CHM_TMR` for the nominal 8-hour NiMH timer. A
populated 10 kOhm `TH1` NTC near the battery contacts drives BQ `TS` directly;
the RP2354B cannot read that analog node. LTC2959 die temperature is nearby
board telemetry, not an exact pack-temperature measurement.

Headset insertion comes from the TCA8418 input. The hook button is measured on
GP41/ADC1 with calibrated Schmitt thresholds. The product does not support SIM
hot-plugging because the SIM is under the battery.

## Service USB

GP28 senses service VBUS directly as a digital input; it is not an ADC pin on
RP2354B. The CDC-enabled service image uses it as a powered-off and powered-on
wake source. The release image uses it only after a Power wake to qualify the
5-second BOOTSEL gesture.

RP USB data and Telit USB data share the battery-bay service connector but are
separate USB PHYs. The assembled prototype requires the ESD-device common-pin
rework documented in `docs/revb2_usb_vbus_backfeed_erratum.md`; without it the
powered Telit USB PHY can backfeed the service VBUS net.

## Known Hardware Limits

- There is no direct SIM-presence contact. Firmware can present missing-SIM UI
  only after Telit supplies conclusive SIM evidence; immediate pre-logo SIM
  detection requires additional hardware sensing.
- GP43 is an analog fallback for charger presence on a full pack. Eliminating
  periodic detection latency requires an interrupt-capable comparator.
- Pack capacity starts from a 1225 mAh product prior. Firmware can learn
  qualified delivered capacity, but LTC2959 does not learn it by itself. An
  observed/learned non-conflicted estimate plus coherent SOC is production bar
  authority. Estimate exhaustion alone is never EMPTY authority; overrun remains
  measurable until a physically qualified endpoint.
- The phone's empty-battery action is soft-off. The CDC-enabled service image
  deliberately lets attached USB veto P1.7 for diagnostics. The release image
  has no such veto and can enter P1.7 with the service cable attached.

## Bench Acceptance

The current Rev B2 firmware has qualified:

- LCD, keypad, vibra, backlight hardware outputs, and power button
- earpiece, headset, microphone, buzzer, DVI voice, and audio resync
- Nokia charger insertion/removal/full behavior, exact coulomb-target cutoff,
  and attached maintenance restart
- LTC2959 voltage/current and shared-alert handling, health-supervisor battery
  bars, and durable natural EMPTY shutdown
- RTC alarm wake, snooze, dismissal, and powered-off wake
- service VBUS and powered-on/off dormant wake paths (CDC-enabled image)
- Telit startup, SIM handling, registration, SMS/RI wake, calls, hold/waiting,
  graceful shutdown, DTR/CTS sleep, and dynamic antenna tuning
- registered one-hour standby at a calibration-revision-2 ACR average of
  16.710 mA, with modem CTS-sleep residency above 98 percent on the measured
  setup

See focused subsystem documents for detailed procedures, measurements, and
qualification scope, including the
[powered-on standby contract](standby_power.md).
