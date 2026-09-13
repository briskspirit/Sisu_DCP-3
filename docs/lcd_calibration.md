# LCD calibration and power-state behavior

Status: implemented and bench-calibrated on 2026-07-26. The replacement-panel
value is provisional across seasons and extreme temperatures, but it has passed
the observed cold-to-warm interval described below.

## 1. Locked distinctions

Three values must not be conflated:

1. **Nokia factory fallback:** raw Vop `63`, TC1, bias 4. This is the
   reverse-engineered Nokia 3210 v6.00 behavior and remains the default for a
   blank or corrupt settings store.
2. **Current aftermarket-panel calibration:** raw Vop `54`, TC1, bias 4.
   This value is saved on the bench DUT and is the qualified starting point for
   the aftermarket panel tested below.
3. **Temperature slope:** still TC1. TC2/TC3 are not qualified; changing the
   slope requires instrumented hot/cold evidence.

The firmware stores raw Vop rather than only Nokia's five-bit level. This keeps
the original formula available while allowing replacement glass or compatible
controllers to use the complete 7-bit `0..127` register domain.

## 2. Nokia v6.00 ground truth

The source of truth is the original NSE-8 v6.00 firmware (disassembly of a
ROM dump), not an older clone document or simulator:

- Observed LCD initialization command stream:
  `21 05 14 BF 20 0C`.
- Decoding:
  - `0x21`: active, extended instruction set.
  - `0x05`: temperature coefficient 1.
  - `0x14`: bias system 4.
  - `0xBF`: raw Vop 63.
  - `0x20`: active, basic instruction set.
  - `0x0C`: normal display mode.
- The configuration writer at ROM `0x002B1DB0` sends TC, bias, and Vop in that
  order.
- Callers at `0x002A8FF2` and `0x0025FFC4` supply TC1 and bias 4.
- Nokia derives raw Vop as `47 + (EEPROM[0x0153] & 0x1F)`.
- The virgin `nse-8.bin` EEPROM byte is `0x10`, giving level 16 and raw Vop 63.
- Nokia's representable level range is therefore raw Vop `47..78`.

Firmware preserves that exact register order and default profile. Runtime
calibration reapplies the same sequence without issuing a hardware reset, so
display RAM survives ordinary reconfiguration.

## 3. Controller interpretation

For a genuine PCD8544 at room temperature, the nominal relationship is:

`VLCD = 3.06 V + Vop * 0.06 V`

Useful points:

| Raw Vop | Nominal VLCD | Meaning here |
|---:|---:|---|
| 44 | 5.70 V | Earlier warm-only candidate; superseded |
| 54 | 6.30 V | Accepted aftermarket-panel baseline |
| 63 | 6.84 V | Nokia v6.00 blank-store fallback |

The nominal temperature coefficients are TC0 `1 mV/K`, TC1 `9 mV/K`, TC2
`17 mV/K`, and TC3 `24 mV/K`, with more LCD voltage applied as temperature
falls. These are first-order estimates only: replacement glass and compatible
controller dies may not follow the Philips transfer curve exactly.

## 4. Bench history and accepted value

Panel under test: aftermarket Nokia 3210 replacement display on an earlier
development DUT. No glass-temperature probe was attached, so the result is
empirical rather than a characterized temperature curve.

1. With the assembled phone heat-soaked, TC1/bias4 and raw Vop `44` initially
   gave a clean-looking image.
2. After roughly one hour of cooling, the same `44` setting was visibly faint.
3. The cold comparison sequence was:
   `56 -> 63 -> 52 -> 56 -> 52 -> 54 -> 56 -> 52 -> 54`.
4. Raw Vop `54`, TC1, bias 4 gave the preferred cooled appearance.
5. The phone was then left running and the screen continued to look good as it
   warmed during the observation window.
6. Raw Vop `54` was explicitly saved and read back from the settings service.

An original Nokia display was briefly installed as a control, but that sample
was itself extremely faint and visibly poor. It is not valid evidence for
comparing the Sisu 3.3 V display rail with Nokia's approximately 2.8 V rail.

### Temperature estimate

The warm candidate `44` and cold candidate `54` differ by ten Vop steps, or a
nominal 0.60 V. Because both observations already used TC1, the coefficient
that would absorb the full manual shift is:

`required coefficient = 9 + 600 / delta_temperature_C` mV/K

TC3 would exactly absorb the ten-step shift only if the panel-temperature
difference were approximately 40 C. For a 20 C, 25 C, or 30 C difference, TC3
would leave approximately 5, 3.75, or 2.5 Vop steps of residual drift. This is
why TC3 is an untested alternative, not a conclusion from the current
uninstrumented test.

## 5. Persistence policy

`STORE_SETTING_SYSTEM_LCD_TUNING` stores the complete Vop/TC/bias tuple in one
`u32`, so a saved calibration is applied atomically on the next boot:

```text
bits 0..6   raw Vop (0..127)
bits 7..8   temperature coefficient (0..3)
bits 9..11  bias system (0..7)
```

- Missing or corrupt tuple: raw Vop `63`, TC1, bias 4.
- Current bench DUT: raw Vop `54`, TC1, bias 4.
- Every valid active tuple can be saved after an explicit two-press Net Monitor
  confirmation; previews remain volatile until that confirmation completes.
- The older `STORE_SETTING_SYSTEM_LCD_VOP` byte remains a migration mirror.
  A non-stock legacy Vop is promoted into a still-stock tuple, and new saves
  mirror their Vop back so temporarily flashing an older build does not lose
  the panel baseline.

The accepted bench tuple remains `54/TC1/bias4`. Persistence supports TC and
bias so a separately temperature-qualified profile would not require another
storage format change; their wider range is a calibration facility, not a claim
that a different slope has already been qualified.

## 6. Adjustment interfaces

### CDC console

- `lcd status`: report active/stored Vop, Nokia-level mapping, TC, bias, and
  controller power state.
- `lcd contrast <0-127>`: volatile raw-Vop preview preserving active TC/bias.
- `lcd temp <0-3>`: volatile TC preview.
- `lcd bias <0-7>`: volatile bias preview.
- `lcd set <vop> <tc> <bias>`: volatile complete-profile preview.
- `lcd cal <0-31>`: preview Nokia's `47 + level` formula with TC1/bias4.
- `lcd stock`: preview Nokia level 16 (`63/TC1/bias4`).
- `lcd reinit`: reapply the active configuration without resetting RAM.
- `lcd save`: persist the complete active Vop/TC/bias tuple.

The full raw domain is exposed for controlled bench work, not as a claim that
every value is panel-safe: Vop 0 disables the charge pump, and the PCD8544
datasheet warns against configurations that drive VLCD above its limit.

### Net Monitor

Page 95 owns the complete calibration workflow:

- `1`/`3`: raw Vop -/+ 1;
- `4`/`6`: TC -/+ 1;
- `7`/`9`: bias -/+ 1;
- `0`: restore the stored tuple;
- `#`: preview Nokia's stock `63/TC1/bias4` tuple; and
- first `*`: capture the active tuple, temporarily show stock contrast, and arm
  a five-second visible confirmation;
- second `*`: reapply and save the exact captured tuple.

Every ordinary preview auto-reverts after ten seconds without another edit.
Changing page, leaving standby, an expired confirmation, or any non-confirming
edit also cancels persistence and reverts or continues from the captured target
as appropriate. Each adjustment is RAM-only, so calibration experiments do not
write one flash journal entry per button press.

The full `0..127` domain remains available for controlled panel work. If a bench
operator deliberately confirms a tuple that makes the display unreadable,
connect CDC and run `lcd stock` followed by `lcd save`; this restores and
persists the visible Nokia fallback without relying on the LCD.

## 7. LCD power-down behavior

LCD sleep is independent of RP dormant sleep and USB CDC:

1. In `APP_ROUTE_POWER_OFF` without an active powered-off charging session or
   a terminal shutdown-error screen, clear all 504 bytes of display RAM. A
   charger that remains attached after charge completion does not retain a
   static full-battery icon.
2. Send PCD8544 command `0x24`.
3. Leave chip select high after the command.
4. USB may keep the RP and CDC console awake; it no longer keeps the LCD active.

Power-on, alarm UI, or powered-off charging calls `lcd_power_up()`, reapplies
TC/bias/Vop plus normal display mode, and forces a complete framebuffer redraw.
A subsequent RP dormant path may send `0x24` again; the operation is idempotent.

## 8. Revisit criteria

Re-open calibration when any of these is observed:

- blank pixels become visible after a sustained warm soak;
- black pixels fade after a sustained cold soak;
- a different replacement-panel batch behaves materially differently;
- a known-good original panel becomes available for a controlled comparison.

Any temperature requalification must record the actual glass or COG temperature
at both endpoints, compare TC1 and TC3 from the same mid-temperature appearance
with bias 4 fixed, and repeat both hot and cold endpoints without changing Vop.
