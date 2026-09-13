# Rev B2 Charger-Enable Recovery Erratum

Status: firmware mitigated and the bench DUT reworked. Any unreworked Rev B2
assembly still requires R77 for unconditional recovery from a deeply depleted
pack.

## Circuit Fact

`CHR_EN_N` connects TCA8418 COL5 to BQ25171 `/CE`. The Rev B2 KiCad source
defines R77 as a 10 kOhm pull-down, but marks it DNP. Board 1 was manufactured
without that external pull-down; the current bench DUT has since received the
10 kOhm rework.

This matters before firmware owns the expander:

- BQ25171 `/CE` is active-low and has only a weak internal pull-down
  (3.3 MOhm nominal).
- TCA8418 GPIOs reset as inputs with their internal pull-ups enabled
  (about 105 kOhm typical).
- The TCA pull-up dominates the BQ pull-down, so reset/default COL5 disables
  charging.

If the battery is too depleted to establish a healthy 3.3 V firmware domain,
software cannot reach the point that drives COL5 low. This explains a charger
that remains at zero input current until battery removal/reinstallation changes
the startup conditions.

## Firmware Mitigation

Firmware now treats COL5 as a verified safety output:

1. TCA initialization latches COL5 high (charge disabled) until the charger
   arbiter applies the persisted owner mask, disables its pull-up, makes it an
   output, and reads back output/direction/pull configuration.
2. Charger enable/disable operations repeat that ordered configuration and
   verify the expander register state. This is not direct pin-voltage sensing.
3. The charger arbiter owns a desired COL5 state and retries it once per second
   while the CPU is already running. A POR-like runtime mismatch invalidates the
   TCA configuration and schedules full reinitialization with that retained
   target; a transient I2C failure cannot restart an inhibited charge or lose a
   pending enable.
4. Board diagnostics expose charger-enable validity separately from the last
   value; NetMonitor displays `?` when readback is unavailable.
5. Entering phone soft-off restores charge-enable so a temporary NetMonitor
   override cannot strand the next charge attempt.

These measures cover a running, adequately powered RP/TCA. They cannot correct
the no-firmware boot interval.

## Hardware Disposition

- Rev B2 assemblies: fit R77 with its designed 10 kOhm value. The bench DUT is
  already reworked.
- Next PCB revision: make the `/CE` pull-down fitted by default and preserve
  active-low, fail-enabled behavior without MCU or expander power.
- Bench acceptance after rework: discharge below the normal phone power-on
  floor, leave service USB detached, connect the Nokia charger, and verify input
  current starts without removing the battery. Repeat with the RP held in reset.

Do not qualify this erratum solely with a normally charged pack: firmware can
mask the missing resistor once 3.3 V and I2C are already healthy.

## Post-Rework Bench Boundary

R77 was fitted and the fail-enabled path was confirmed, but one narrower
recovery boundary remains unresolved:

- After an overnight discharge ended in an RP2350 brownout, the removed pack
  measured 1.84 V. With service USB attached, connecting the charger drew only
  12--13 mA at its input and did not begin normal charging. Disconnecting USB
  allowed charging to start immediately.
- In a later controlled run, the same reworked board was still alive in soft
  off at 2.170 V. Normal charging started with service USB left attached.

Therefore service USB is not generally incompatible with charging. The first
observation is specific to the post-brownout, deeply depleted startup boundary;
its mechanism has not yet been established. Do not fold it into normal charger
policy or add a firmware workaround without reproducing it while observing the
RP reset state, 3.3 V rail, TCA8418 power/configuration, and physical `/CE`.
