# Telit Shutdown Recovery

Status: **implemented, host-tested, and qualified on the Rev B2 bench**.

This document owns the production recovery policy for a Telit module that does
not complete an ordinary software shutdown. The normal path and both fallbacks
are finite, preserve the newest power intent, and never use a timeout as
permission to remove module power.

## Hardware And Vendor Contract

Rev B2 routes the two module controls through independent 2N7002 low-side
gates:

- RP GP38 high asserts Telit `ON_OFF_N` low;
- RP GP39 high asserts Telit `HW_SHUTDOWN_N` low;
- RP GP40 measures Telit PWRMON/VAUX;
- RP GP4 reads the +3.8 V regulator power-good output.

The FETs give Telit's required open-collector behavior. Firmware keeps both RP
outputs low unless a bounded pulse is active. The LE910Cx hardware guide defines
three shutdown mechanisms:

1. `AT#SHDN`, whose early `OK` is not completion. The AT guide permits up to
   25 seconds before switch-off.
2. `ON_OFF_N` low for at least 2.5 seconds. Finalization has no stated maximum
   and usually takes more than 15 seconds.
3. Emergency-only `HW_SHUTDOWN_N` low for at least 200 ms.

For every mechanism, PWRMON low is the module-off evidence. Rev B2 qualifies it
as raw GP40 ADC <= 64 continuously for 500 ms. No command final, elapsed timer,
PG transition, UART level, or control pulse can substitute for that evidence.

## Production Algorithm

An accepted power-off request executes exactly this sequence:

1. Send `AT#SHDN`; wait up to 3 seconds for its final, then observe PWRMON for
   25 seconds whether the final was `OK` or missing.
2. If PWRMON has not qualified low, pulse GP38 for 3000 ms, release it, reset the
   low qualifier, and observe PWRMON for 60000 ms. The minute is a conservative
   firmware policy value because Telit gives no hardware-finalization maximum.
3. If PWRMON still has not qualified low, pulse GP39 for 250 ms, release it,
   reset the low qualifier, and observe PWRMON for 5000 ms.
4. If PWRMON still does not qualify low, enter one terminal failure state. Keep
   the modem's +3.8 V ownership, continue the authorized 250 ms PWRMON polling,
   deassert both controls, and show `Error in connection` on the powered-off
   screen with the backlight on. A normal long power-key press retries recovery.
   If PWRMON later qualifies low, release the rail and return immediately to the
   ordinary dark powered-off screen.

There is no automatic pulse loop. Failure to allocate the hardware pulse alarm
also goes directly to the fail-closed terminal state; firmware never substitutes
an unbounded software-timed assertion.

If a newer power-on request arrives after shutdown is on wire, shutdown remains
irreversible. A successful shutdown performs a real rail-down dwell and restarts
once. If all shutdown stages fail, the newer intent instead recovers the
still-powered module through the ordinary PWRMON, RX-idle, CTS, AT, init, and
provisioning gates. It does not cut or repulse the rail.

## Pulse Ownership

`modem_service` owns stage selection and deadlines from neutral
`modem_power_cfg_t` data. `modem_uart_hal` owns the physical GP38/GP39 pulse and
arms a Pico hardware alarm before reporting success. The alarm callback
deasserts the selected control independently of main-loop progress. Every OFF,
FAILED, startup, and recovery boundary synchronously cancels any remaining
alarm and drives both controls inactive. The service's coarse sequencing
deadline is one millisecond later than the requested pulse width, so timestamp
truncation can never make synchronous cleanup release a pin before its alarm.

The Telit command and timing data remain in `modem_vendor_telit.c`; no Telit pin
or command leaks into app code. The app sees only a one-shot neutral terminal
power-off failure indication.

## Diagnostics

Net Monitor page 28 has three frames. Its third frame records:

- `SHDN`: current stage (`0` none, `1` software, `2` GP38 graceful, `3` GP39
  unconditional, `4` terminal failure);
- `F`: terminal-failure state;
- `HW`: lifetime graceful hardware pulse count;
- `EMERG`: lifetime unconditional pulse count;
- `TERM`: lifetime terminal-failure count.

CDC logs timestamp every stage start, pulse release, qualified completion, and
terminal failure. The standalone `diag_telit` image additionally exposes raw
10 ms GP40 samples, 500 ms PG/PWRMON/LTC reports, exact pulse timestamps, and
two-step GP39 arming.

## Host Evidence

`test_modem_telit_service` covers:

- ordinary software shutdown without fallback;
- stuck software shutdown recovered by one GP38 pulse;
- failed GP38 shutdown recovered by one GP39 pulse;
- both controls ineffective, producing one visible terminal failure while the
  high-PWRMON rail remains owned;
- a late qualified-low PWRMON completing that terminal state;
- hardware-alarm allocation failure without a blind emergency attempt;
- power-on during shutdown surviving complete ladder exhaustion;
- no accidental inheritance by an uncommanded runtime PWRMON drop.

`test_power_app` pins the one-shot powered-off UI handoff. Net Monitor rendering
pins the complete third frame.

## Rev B2 Evidence

The assembled-board qualification covered ordinary `AT#SHDN`, the GP38
graceful fallback, the GP39 emergency fallback, and fail-closed rail retention.
Every successful path observed 500 ms qualified-low PWRMON before releasing the
modem rail. The normal path passed before and after a call; the return-to-service
run also passed a two-way audio call and a sleeping-SMS wake with zero production
UART error counters.

The GP38 and GP39 pulses were hardware-alarm timed and their effects were
confirmed by PWRMON on the assembled module. They were not independently scoped,
so the evidence qualifies the complete board behavior rather than metrology of
the pulse widths themselves.
