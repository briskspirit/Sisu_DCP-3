# Audio calibration recorder

This localhost-only tool records unprocessed mono PCM from a selected microphone,
stores each take as a 24-bit WAV, and compares the original Nokia 3210 against
the Sisu DUT without changing the microphone gain between takes.

## Run

```sh
python3 tools/audio_calibration/server.py
```

Open `http://127.0.0.1:8765`, select the Rode input, and grant microphone
permission. The browser is asked to disable echo cancellation, noise
suppression, and automatic gain control. Confirm those settings in the input
status shown by the page.

Each take lasts 15 seconds so an approximately eight-second Nokia tune fits in
one file with clean room before and after it. Start a tune about one second
after capture begins and let the complete melody finish. Held-DTMF continuous
RMS uses the early steady window from 1.5 to 3.5 seconds; this stays ahead of
the original phone's roughly 3.84-second key-hold cutoff. Active-frame analysis
for repeated key tones uses the wider 1.5-to-14.5-second interval. Hold the same
DTMF digit before starting each take and keep holding it until the handset's own
cutoff.

Recordings and their JSON metadata are written to `recordings/`, which is
ignored by Git.

## DUT bench controls

The CDC service firmware exposes volatile calibration controls. They reset at
boot and do not alter stored settings:

```text
keycal status
keycal reset
keycal click <gain-percent> <third-harmonic-percent> <phase-degrees> <attack-ms> <release-ms>
keycal harmonic <2|3|4|5> <percent> <phase-degrees>
keycal dtmf <gain-percent> <low-weight> <high-weight>
keycal play click
keycal play 2 3840
keycal stop
hpgain <0-63>
buzzcal status
buzzcal duty <1-50|auto>
buzzcal play [1-5]
buzzcal stop
```

`keycal reset` restores the measured production defaults: click gain 93%, a
1% third harmonic at +92 degrees, a 6 ms attack and 4 ms release across the
96 ms click, and DTMF gain 120% with handset weights 74:100 and headset weights
54:100. Both codec output stages use a neutral 0 dB baseline. The local PCM path
applies -30 dB on the headset route before mixing with modem voice; `keycal
dtmf` changes the currently selected route's spectral weights only. `play` uses
the active profile's keypad-tone level and lets the DUT be recorded without a
mechanical keypad strike.

The `harmonic` control is intentionally volatile and click-only. It exists to
pre-compensate the acoustic H2-H5 distortion at the final loudness without
changing DTMF, call audio, or other system tones.

`buzzcal duty` is an exact, volatile override used to sweep the fitted magnetic
buzzer. `buzzcal play` uses Nokia Tune and the real ringtone-preview envelope;
omitting its level selects Level 5. Restore `buzzcal duty auto` after a sweep.

## 2026-08-30 first measurements

Both phones used keypad tones Level 3 and the same Rode PodMic input:

- The generic click is 900 Hz on both phones, agreeing with the v6.00 ROM's
  tone-00 mode-2 frequency word and 0x0c duration byte.
- The stock click sustains almost flat for roughly 90 ms before its short
  release. The DUT's legacy envelope decays throughout all 96 ms.
- In steady click bodies, stock H3 is about -22.4 dBc while the DUT's pure-sine
  path measures about -36.1 dBc. The relative H3 phase also differs by about
  42 degrees, hence the independent magnitude and phase controls.
- Held DTMF-2 is exactly 697/1336 Hz on both. Stock twist is about +11.6 dB;
  the DUT is about +18.3 dB. An early model-derived setting used gain 60% and
  weights 74:100, which preserves the already-correct low component while
  reducing the high component by about 6.7 dB.
- Two clean stock long-hold captures stop after 3.861 s and 3.883 s. The v6.00
  tone-03 bytecode itself loops while the lower keyboard state reports the key
  held, so the fixed policy is below the decoded tone/UI layer. The production
  ceiling uses 480 of the stock 8 ms scheduler ticks (3.840 s), matching those
  recordings. A held C produces a second click when clear-all fires; its
  click-onset spacing is about 497 ms.

The waveform constants were selected from controlled same-take sweeps:

- Click H3 measured -22.23 dBc on the DUT versus -22.42 dBc on the Nokia; its
  relative phase was within 1.7 degrees. Attack and release envelope errors
  were 0.019 and 0.032 normalized RMS respectively.
- DTMF-2 measured 697/1336 Hz. Three valid DUT repeats measured +12.30 to
  +13.19 dB twist, spanning the usable stock repeats (+11.66 and +12.65 dB).
  The strongest non-fundamental product above 500 Hz was -32.1 dBc versus
  -33.5 dBc on the Nokia, with no clipped samples.
- The DUT limiter emits exactly 61,440 samples (3.840 s at 16 kHz), pinned by
  the host audio test. Microphone onset/offset measurements remain slightly
  window-dependent.

Fresh stock recordings then fixed the three keypad-tone levels from the quiet
second tone of each C-hold pair, avoiding the physical key strike:

- Level 2 is -9.04 dB relative to Level 3 (amplitude ratio 0.3530).
- Level 1 is -16.75 dB relative to Level 3 (amplitude ratio 0.1453).
- The keypad-only Q8 trims are therefore 225/256 over audio level 2 and
  149/256 over audio level 1; Level 3 uses audio level 4 at unity. Clicks and
  DTMF share this ladder, while other system sounds retain their own levels.
- At the clean 0 dB codec baseline, a 146% DUT click measured 3.92 dB above
  the fresh stock Level-3 click and was also judged louder by ear. Applying
  that measured correction gives the 93% production value. The click
  waveform itself stays at the already-close 1% H3/+92 degree, 6/4 ms shape.

The DUT uses the same Nokia 3210 earpiece and enclosure as the reference phone,
so these waveform corrections are not compensating for a different receiver.
Absolute level must still be rechecked with each phone's outlet at the same
marked microphone position; a few millimeters at 30-40 mm materially changes
the measured SPL.

## 2026-08-30 buzzer calibration

Five stock and five DUT Nokia Tune previews established the ringtone behavior:

- All 13 DUT note pitches match the stock phone to well under one cent. The ROM
  tone table and note-duration decoder therefore remain unchanged.
- Stock Levels 1..4 are flat for the complete preview. At Level 5, the first
  pass is Level 4 and the second pass rises to Level 5. The ringtone preview now
  uses that Level-4 seed; alarm and generic-alert envelopes retain their own
  seeds.
- Relative to stock Level 4, the measured stock ladder is -11.42, -8.34,
  -4.19, 0, and +9.20 dB for Levels 1..5.

An autonomous, randomized 1..50% duty sweep used one microphone capture and
repeated 10%, 25%, and 50% references. Repeat drift was at most 0.34 dB. The
fitted CMT-1085-85-SMT output rises through the low-duty region, peaks near
24%, and then falls toward 50%; the former assumption that 50% is loudest was
wrong. The production audio-level duties are now 2%, 3%, 5%, 8%, and 24%. A
final capture through the real ringtone-preview command path measured -12.04,
-8.26, -3.67, 0, and +5.15 dB relative to Level 4. Levels 1..4 are each within
0.62 dB of stock, and two independent Level-4 passes agreed within 0.05 dB.

The stock Level-5 jump is about 4.05 dB beyond the maximum acoustic output of
this single-ended 3.8 V buzzer circuit. Firmware uses the measured 24% maximum
instead of overdriving the coil or claiming a louder but actually quieter 50%
setting. The fitted part is rated at 3.6 Vo-p, 2,730 Hz, half-duty square drive;
the rail and waveform must not be raised merely to close that remaining gap.

The DUT's fitted buzzer has a stronger 2.6-2.7 kHz resonance and weaker
2.2-2.35 kHz response than the stock transducer. The production mapping leaves
per-note duty uncompensated because the available evidence does not justify
changing alarms, warnings, game audio, and Nokia Tune independently.

The stock melody body measured about 2.991 s and the DUT about 3.047 s, making
the DUT roughly 1.9% slower, not faster. The perceived faster preview is its
shorter pre-play delay. The production mapping leaves that startup delay as-is;
the evidence does not justify changing the shared ROM-derived note-duration
formula for one UI delay.

## 2026-08-31 call-volume derivation

Stock voice-call SPL could not be compared reliably, so the relative ladders
come from v6.00 and the absolute anchors were checked on a live Telit call:

- The firmware keeps four independent 0..9 call-volume bytes at `0x00110b40`
  and selects the current channel through `0x002a8386`.
- The coefficient banks at `0x002da424` and `0x002da474` are logarithmically
  spaced. The normal handset channels advance by 1.000 dB per level (one bank
  saturates its quietest entries); both external-accessory channels advance by
  2.000 dB per level.
- Handset Levels 1..10 are -9..0 dB in 1 dB steps.
- HDC-5 local synthesized PCM is attenuated -30 dB, where DTMF matches the
  stock phone. The codec output itself remains at a neutral 0 dB baseline, so
  Telit DVI downlink is not accidentally attenuated with it. A live-call sweep
  selected 0 dB for Level 10, making headset call Levels 1..10 -18..0 dB in
  2 dB steps.
- Telit DVI PCM stays unscaled at unity. Call volume uses the codec output
  stage, preserving digital headroom; call teardown restores both outputs to
  neutral 0 dB while the route-specific local PCM calibration remains digital.

The application stores only the Nokia 1..10 level. The audio/codec boundary
derives both cached output gains, so inserting or removing a headset during a
call preserves the selected level without exposing NAU88C22 register values to
the calls application. Core1 services also retain that semantic level across a
runtime codec re-init (including the narrow pre-bridge dispatch window); call
teardown clears the intent before restoring the ordinary tone-path baselines.

## Measurement controls

- Keep microphone gain, browser input, and room position unchanged.
- Mark one physical reference point and put each phone's acoustic outlet at
  that same point. At a 30-40 mm distance, a few millimeters matter.
- Keep the display/backlight and phone profile in the same state for both takes.
- Reject takes with clipping or a room noise interruption.
- Repeat close results at least three times before changing firmware gain.

## Self-test

```sh
node tools/audio_calibration/test_analysis.mjs
```
