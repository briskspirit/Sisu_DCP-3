#include "audio/audio_service.h"
#include "audio/composer_codec.h"

#include "hardware/sync.h"
#include "audio/audio_bridge.h"
#include "audio/audio_i2s_hal.h"
#include "audio/audio_levels.h"
#include "hal/board.h"
#include "audio/buzzer_hal.h"
#include "services/core1_services.h"
#include "generated/tones.h"
#include "audio/modem_i2s_hal.h"
#include "audio/nau88c22_codec.h" /* nau_route_t only -- core1 never touches codec I2C */
#include "services/log.h"
#include "services/timebase.h"
#include "audio/vibra_hal.h"

#include <stdbool.h>
#include <string.h>

#define AUDIO_CLICK_HZ 900u
#define AUDIO_CLICK_MS 96u
#define AUDIO_CLICK_ATTACK_MS_MAX 20u
#define AUDIO_CLICK_ATTACK_SAMPLES_DEFAULT \
    ((AUDIO_SAMPLE_RATE * AUDIO_KEYPAD_CLICK_ATTACK_MS_DEFAULT) / 1000u)
#define AUDIO_CLICK_SAMPLES ((AUDIO_SAMPLE_RATE * AUDIO_CLICK_MS) / 1000u)
#define AUDIO_CLICK_RELEASE_MS_MAX (AUDIO_CLICK_MS - 3u)
#define AUDIO_CLICK_RELEASE_SAMPLES_DEFAULT \
    ((AUDIO_SAMPLE_RATE * AUDIO_KEYPAD_CLICK_RELEASE_MS_DEFAULT) / 1000u)
#define AUDIO_KEYPAD_GAIN_Q8_MAX 512u
#define AUDIO_CLICK_HARMONIC_Q12_MAX 2048
#define AUDIO_CLICK_HARMONIC_PHASE_DEG_MAX 180
#define AUDIO_DTMF_MAX_SAMPLES \
    ((AUDIO_SAMPLE_RATE * AUDIO_KEYPAD_DTMF_MAX_HOLD_MS) / 1000u)
#define AUDIO_TONE_ATTACK_SAMPLES ((AUDIO_SAMPLE_RATE * 3u) / 1000u)
#define AUDIO_TONE_RELEASE_SAMPLES ((AUDIO_SAMPLE_RATE * 6u) / 1000u)
#define AUDIO_TONE_REPEAT_FOREVER 255u
#define AUDIO_TONE_REPEAT_DEPTH 4u
/* Bench trim for v6.00's fixed tone-14 notification during a voice call.
 * Level 1 (5600) still overpowered speech on board #1; half amplitude keeps
 * the notification distinct while preserving the five global UI levels. */
#define AUDIO_QUIET_SYSTEM_TONE_AMPLITUDE 2800
#define AUDIO_VIBRA_TICK_MS 8u
#define AUDIO_VIBRA_PULSE_DEFAULT_TICKS 6u
#define AUDIO_VIBRA_PULSE_LOOP_OFF_TICKS 96u
/* The generic 0x09 volume seed is 6 of the original's 0..10 range (3 on our
 * 0..5 scale), stepped on timer 0x28: 514 * 8 ms ~= 4.1 s per level. Alarm tone
 * 12 uses a lower product-specific seed below: the physical handset confirms
 * that it starts almost silent and traverses about five audible levels. */
#define AUDIO_ENV_SEED_LEVEL 3u
#define AUDIO_RINGTONE_ENV_SEED_LEVEL 4u
#define AUDIO_ALARM_ENV_SEED_LEVEL 1u
#define AUDIO_ENV_STEP_MS (514u * 8u)

typedef enum {
    AUDIO_KIND_SILENCE = 0,
    AUDIO_KIND_CLICK,
    AUDIO_KIND_DTMF,
    AUDIO_KIND_TONE_STREAM,
    AUDIO_KIND_SEQUENCE,
    AUDIO_KIND_COMPOSER_NOTE,
    AUDIO_KIND_COMPOSER_PACKED,
} audio_kind_t;

typedef struct {
    uint16_t start_pos;
    uint8_t remaining;
} tone_repeat_t;

typedef struct {
    uint16_t hz;
    uint16_t duration_ms;
} audio_sequence_step_t;

typedef struct {
    audio_kind_t kind;
    char active_key;
    uint32_t sample_index;
    uint32_t phase0;
    uint32_t phase1;
    uint32_t step0;
    uint32_t step1;
    const uint8_t *tone_data;
    uint16_t tone_len;
    uint16_t tone_start;
    uint16_t tone_pos;
    tone_repeat_t tone_repeats[AUDIO_TONE_REPEAT_DEPTH];
    uint8_t tone_repeat_depth;
    uint8_t tone_restart_count;
    uint32_t tone_segment_total;
    uint32_t tone_segment_remaining;
    uint8_t tone_rest;
    const audio_sequence_step_t *sequence;
    audio_sequence_step_t composer_sequence[96];
    uint8_t sequence_len;
    uint8_t sequence_pos;
    bool sequence_square;
    bool loop; /* COMPOSER_PACKED: replay whole-melody until stopped (own-tone ring) */
    bool dynamics; /* honour the 0x09 volume-seed crescendo for opted-in streams */
    int16_t amplitude;
} audio_state_t;

typedef struct {
    char key;
    uint16_t low_hz;
    uint16_t high_hz;
} dtmf_row_t;

static const int16_t SINE_Q15[65] = {
    0, 804, 1608, 2410, 3212, 4011, 4808, 5602,
    6393, 7179, 7962, 8739, 9512, 10278, 11039, 11793,
    12539, 13279, 14010, 14732, 15446, 16151, 16846, 17530,
    18204, 18868, 19519, 20159, 20787, 21403, 22005, 22594,
    23170, 23731, 24279, 24811, 25329, 25832, 26319, 26790,
    27245, 27683, 28105, 28510, 28898, 29268, 29621, 29956,
    30273, 30571, 30852, 31113, 31356, 31580, 31785, 31971,
    32137, 32285, 32412, 32521, 32609, 32678, 32728, 32757,
    32767,
};

static const dtmf_row_t DTMF_TABLE[] = {
    {'1', 697u, 1209u},
    {'2', 697u, 1336u},
    {'3', 697u, 1477u},
    {'4', 770u, 1209u},
    {'5', 770u, 1336u},
    {'6', 770u, 1477u},
    {'7', 852u, 1209u},
    {'8', 852u, 1336u},
    {'9', 852u, 1477u},
    {'*', 941u, 1209u},
    {'0', 941u, 1336u},
    {'#', 941u, 1477u},
};

static const audio_sequence_step_t PACMAN_DOT0[] = {{980u, 42u}, {740u, 34u}};
static const audio_sequence_step_t PACMAN_DOT1[] = {{740u, 42u}, {980u, 34u}};
static const audio_sequence_step_t PACMAN_POWER[] = {{392u, 42u}, {523u, 42u}, {784u, 58u}};
static const audio_sequence_step_t PACMAN_GHOST[] = {{988u, 44u}, {1319u, 44u}, {1760u, 54u}};
static const audio_sequence_step_t PACMAN_DEATH[] = {{740u, 55u}, {622u, 55u}, {523u, 70u}, {392u, 90u}};
static const audio_sequence_step_t PACMAN_WIN[] = {{523u, 48u}, {659u, 48u}, {784u, 48u}, {1047u, 90u}};

static void audio_fill(void *ctx, int16_t *dst, uint32_t frame_count);
static void start_click(uint8_t level);
static void start_dtmf(char key, uint8_t level);
static void start_system_tone(uint8_t index, uint8_t level, bool loop,
                              bool marker_vibra, int16_t amplitude);
static bool system_tone_uses_buzzer(uint8_t index);
static void start_ringtone(uint8_t index, uint8_t level, bool loop, bool to_buzzer,
                           bool dynamics, bool marker_vibra);
static uint8_t audio_to_buzzer_level(uint8_t level);
static void start_pacman_tone(uint8_t index, uint8_t level);
static void start_composer_note(uint8_t pitch, uint8_t level);
static void start_composer_packed(const uint8_t *data, uint16_t len, uint8_t level, bool loop);
static void start_tone_bytes(const uint8_t *data, uint16_t len, uint8_t level,
                             int16_t amplitude, uint8_t repeats, bool square,
                             bool dynamics, uint8_t env_seed_level,
                             bool marker_vibra);
static void stop_key(char key);
static void stop_composer_preview(void);
static void stop_composer_audio(void);
static void stop_synth_audio(void);
static void stop_all_audio(void);
static void finish_tone_stream(audio_state_t *state);
static void finish_speaker_audio_start(void);
static void start_vibra_pulse(uint16_t arg, bool loop, uint32_t now_ms);
static void stop_vibra_pulse(void);
static uint8_t vibra_arg_strength(uint16_t arg);
static uint8_t vibra_arg_ticks(uint16_t arg);
static const dtmf_row_t *find_dtmf(char key);
static uint32_t phase_step(uint16_t hz);
static int16_t sine_q15(uint32_t phase);
static int32_t click_wave_q15(uint32_t phase);
static int32_t click_envelope_q8(uint32_t sample_index);
static int32_t headset_local_sample(int32_t sample);
static uint16_t tone_start_offset(const uint8_t *data, uint16_t len);
static bool tone_next_segment(audio_state_t *state);
static bool sequence_next_segment(audio_state_t *state);
static uint32_t tone_duration_samples(uint8_t units);
static int16_t audio_level_amplitude(uint8_t level);
static int16_t keypad_audio_amplitude(uint8_t level);
static bool composer_packed_decode(const uint8_t *data,
                                   uint16_t len,
                                   audio_sequence_step_t *steps,
                                   uint8_t step_cap,
                                   uint8_t *out_count);
static uint16_t composer_packed_duration_ms(uint8_t tempo, uint8_t duration_code, uint8_t dotted);
static uint16_t composer_packed_pitch_hz(uint8_t octave, uint8_t pitch_code);

static audio_state_t s_audio;
/* Gate for ringtone-bytecode vibra markers. It is installed atomically with
 * each stream start from that command's audio argument; the decoder therefore
 * never needs core0-only profile state and an old stream cannot clear a new
 * stream's separately queued arm operation. */
static bool s_vibra_enabled;
static bool s_vibra_pulse_active;
static bool s_vibra_pulse_loop;
/* Debug output tests may be preempted by production audio. Their bounded cleanup
 * must stop only a path that is still owned by the test, never a later ring. */
static bool s_debug_buzzer_owned;
static bool s_debug_vibra_owned;
/* Tones value-picker previews are replaced by ordinary keypad clicks before
 * the picker handler posts cleanup. Track that ownership explicitly so the
 * cleanup stops only a preview that is still current, never the newer click. */
static bool s_tones_preview_owned;
static uint8_t s_vibra_pulse_ticks = AUDIO_VIBRA_PULSE_DEFAULT_TICKS;
static uint8_t s_vibra_pulse_strength = VIBRA_HAL_STRENGTH_STOCK;
static uint32_t s_vibra_pulse_until_ms;
static uint32_t s_vibra_pulse_next_ms;
/* Ascending-ring ramp state (core1-only): active during an Ascending incoming
 * ring; steps the buzzer drive level from FLOOR up to TOP on the ramp timer. */
/* Crescendo state (core1-only): an opted-in stream's 0x09 seed arms this; the
 * level steps toward s_env_ceiling, driving I2S amplitude and, for square-role
 * alerts, the magnetic-buzzer duty. */
static bool s_env_active;
static uint8_t s_env_level;
static uint8_t s_env_ceiling;
static uint8_t s_env_seed_level = AUDIO_ENV_SEED_LEVEL;
static uint32_t s_env_next_step_ms;
/* M7 ring lights: published by core1 (tone op 0x0a operand 0x01 on / 0x0b,0xf7,
 * 0xfe off), mirrored onto the backlight by core0. NOT gated by the vibra setting
 * -- v6.00 flashes the keypad/display lights on every ring regardless of vibra. */
static volatile bool s_ring_light;
/* Published by core1, read locklessly by core0 (see audio_service_is_active):
 * core0 defers flash commits while this is set so a flash erase can't park
 * core1 mid-playback and starve the audio DMA refill. */
static volatile bool s_audio_active;
/* Core1-only: whether this backend has an implemented digital voice path. */
static bool s_modem_voice_transport_available;
/* core1-only: true once modem BCLK has been seen running in the current bridge
 * session, gating the BCLK-loss fallback teardown so it can't fire during the
 * call-connect gap before the modem starts its dynamic clocks. */
static bool s_bridge_saw_bclk;
/* A call-state edge is only intent. Keep the bridge muted until the BCLK
 * detector sees continuously running clocks from this session, then resync.
 * Resyncing at the first edge can still catch the modem's startup transient: bench
 * showed clean downlink but a bit-shifted uplink until a later manual restart. */
static bool s_bridge_start_pending;
static bool s_bridge_bclk_qualifying;
static uint32_t s_bridge_bclk_since_ms;
static volatile bool s_diag_bridge_start_pending;
static volatile bool s_diag_bclk_qualifying;
static volatile uint32_t s_diag_bridge_start_requests;
static volatile uint32_t s_diag_bridge_activations;
static volatile uint32_t s_diag_bridge_stop_requests;
static volatile uint32_t s_diag_bridge_bclk_losses;
static volatile uint32_t s_diag_bridge_resync_failures;
static volatile uint32_t s_diag_bridge_last_acquire_ms;
static volatile uint32_t s_diag_bridge_max_acquire_ms;
static volatile uint32_t s_diag_bridge_last_request_ms;
static volatile uint32_t s_diag_bridge_last_active_ms;
static volatile uint32_t s_diag_bridge_last_stop_ms;

/* 20 ms is inaudible against call setup and spans hundreds of complete I2S
 * frames. Every poll must contain fresh detector output; a single startup edge
 * followed by the liveness holdover cannot satisfy the gate. */
#define AUDIO_BRIDGE_BCLK_STABLE_MS 20u

void audio_service_init(bool modem_voice_transport_available) {
    s_modem_voice_transport_available = modem_voice_transport_available;
    s_bridge_saw_bclk = false;
    s_bridge_start_pending = false;
    s_bridge_bclk_qualifying = false;
    s_diag_bridge_start_pending = false;
    s_diag_bclk_qualifying = false;
    s_diag_bridge_start_requests = 0u;
    s_diag_bridge_activations = 0u;
    s_diag_bridge_stop_requests = 0u;
    s_diag_bridge_bclk_losses = 0u;
    s_diag_bridge_resync_failures = 0u;
    s_diag_bridge_last_acquire_ms = 0u;
    s_diag_bridge_max_acquire_ms = 0u;
    s_diag_bridge_last_request_ms = 0u;
    s_diag_bridge_last_active_ms = 0u;
    s_diag_bridge_last_stop_ms = 0u;
    s_debug_buzzer_owned = false;
    s_debug_vibra_owned = false;
    s_tones_preview_owned = false;
    audio_bridge_init();
    if (audio_i2s_hal_init(audio_fill, &s_audio)) {
        LOGI("audio", "I2S DMA ready on GP%u/%u/%u", AUDIO_PIN_BCLK, AUDIO_PIN_LRC, AUDIO_PIN_DIN);
    } else {
        LOGE("audio", "I2S DMA init failed");
    }
    if (!s_modem_voice_transport_available) {
        LOGI("audio", "modem voice transport disabled");
    } else if (modem_i2s_hal_init()) {
        LOGI("audio", "modem voice I2S configured and parked on GP%u/%u/%u",
             MODEM_I2S_PIN_CLK,
             MODEM_I2S_PIN_WA,
             MODEM_I2S_PIN_TXD);
    } else {
        s_modem_voice_transport_available = false;
        LOGE("audio", "modem voice I2S init failed; bridge disabled");
    }
}

/* Recompute the core0-visible "audio active" flag from the current synth/vibra
 * state. Called whenever that state can change (command dispatch, composer
 * start, and the 1 ms tick which catches natural tone end and vibra timeout). */
static void audio_update_active_flag(void) {
    /* s_vibra_pulse_loop keeps this set through the loop's off-gaps (a silent
     * vibrate alert), not just while the motor is energized. */
    s_audio_active = (s_audio.kind != AUDIO_KIND_SILENCE) || s_vibra_pulse_active ||
                     s_vibra_pulse_loop || audio_bridge_active() ||
                     s_bridge_start_pending || modem_i2s_hal_running();
}

bool audio_service_is_active(void) {
    return s_audio_active;
}

bool audio_service_ring_light(void) {
    return s_ring_light;
}

void audio_service_bridge_start(void) {
    uint32_t now_ms = time_ms();
    s_diag_bridge_start_requests++;
    s_diag_bridge_last_request_ms = now_ms;
    audio_bridge_stop();
    s_bridge_saw_bclk = false;
    s_bridge_bclk_qualifying = false;
    if (!s_modem_voice_transport_available) {
        s_bridge_start_pending = false;
        s_diag_bridge_start_pending = false;
        s_diag_bclk_qualifying = false;
        audio_update_active_flag();
        return;
    }
    if (!modem_i2s_hal_start()) {
        s_bridge_start_pending = false;
        s_diag_bridge_start_pending = false;
        s_diag_bclk_qualifying = false;
        s_diag_bridge_resync_failures++;
        audio_update_active_flag();
        return;
    }
    s_bridge_start_pending = true;
    s_diag_bridge_start_pending = true;
    s_diag_bclk_qualifying = false;
    modem_i2s_hal_bclk_reset();
    audio_update_active_flag();
}

void audio_service_bridge_stop(void) {
    s_diag_bridge_stop_requests++;
    s_diag_bridge_last_stop_ms = time_ms();
    s_bridge_start_pending = false;
    s_bridge_bclk_qualifying = false;
    s_diag_bridge_start_pending = false;
    s_diag_bclk_qualifying = false;
    audio_bridge_stop();
    s_bridge_saw_bclk = false;
    (void)modem_i2s_hal_stop();
    audio_update_active_flag();
}

void audio_service_tick(uint32_t now_ms) {
    if (s_vibra_pulse_active && time_diff_ms(now_ms, s_vibra_pulse_until_ms) >= 0) {
        vibra_hal_set(false);
        s_vibra_pulse_active = false;
        if (s_vibra_pulse_loop) {
            s_vibra_pulse_next_ms = now_ms + (uint32_t)AUDIO_VIBRA_PULSE_LOOP_OFF_TICKS * AUDIO_VIBRA_TICK_MS;
        }
    }
    if (s_vibra_pulse_loop && !s_vibra_pulse_active &&
        time_diff_ms(now_ms, s_vibra_pulse_next_ms) >= 0) {
        vibra_hal_set_strength(s_vibra_pulse_strength);
        vibra_hal_set(true);
        s_vibra_pulse_active = true;
        s_vibra_pulse_until_ms = now_ms + (uint32_t)s_vibra_pulse_ticks * AUDIO_VIBRA_TICK_MS;
    }

    /* Step the running level toward the ceiling once per timer-0x28 interval.
     * Square-role streams apply the same level to the magnetic-buzzer duty. */
    if (s_env_active && s_audio.kind == AUDIO_KIND_TONE_STREAM &&
        s_env_level < s_env_ceiling &&
        time_diff_ms(now_ms, s_env_next_step_ms) >= 0) {
        s_env_level++;
        s_env_next_step_ms = now_ms + AUDIO_ENV_STEP_MS;
        s_audio.amplitude = audio_level_amplitude(s_env_level);
        if (s_audio.sequence_square && buzzer_hal_available()) {
            buzzer_hal_set_level(audio_to_buzzer_level(s_env_level));
        }
    }

    /* A call-state edge can precede the modem's dynamic I2S clock. Qualify continuous
     * fresh detector output before re-arming DMA/PIO: one edge is not enough,
     * because a clock-start transient can align RX while leaving TX shifted.
     * The full resync also resets both elastic rings, so no pre-lock samples
     * become audible. */
    bool bclk_fresh = false;
    bool bclk_active = false;
    if (s_modem_voice_transport_available) {
        bclk_active = modem_i2s_hal_bclk_poll(&bclk_fresh);
    }
    if (s_modem_voice_transport_available && s_bridge_start_pending) {
        if (!bclk_fresh) {
            s_bridge_bclk_qualifying = false;
            s_diag_bclk_qualifying = false;
        } else if (!s_bridge_bclk_qualifying) {
            s_bridge_bclk_qualifying = true;
            s_diag_bclk_qualifying = true;
            s_bridge_bclk_since_ms = now_ms;
        } else if (time_diff_ms(now_ms, s_bridge_bclk_since_ms) >=
                   (int32_t)AUDIO_BRIDGE_BCLK_STABLE_MS) {
            if (modem_i2s_hal_resync()) {
                audio_bridge_start();
                s_bridge_start_pending = false;
                s_bridge_bclk_qualifying = false;
                s_diag_bridge_start_pending = false;
                s_diag_bclk_qualifying = false;
                s_diag_bridge_activations++;
                uint32_t acquire_ms = now_ms - s_diag_bridge_last_request_ms;
                s_diag_bridge_last_acquire_ms = acquire_ms;
                if (acquire_ms > s_diag_bridge_max_acquire_ms) {
                    s_diag_bridge_max_acquire_ms = acquire_ms;
                }
                s_diag_bridge_last_active_ms = now_ms;
                s_bridge_saw_bclk = true;
            } else {
                /* Never arm the mixer over a half-stopped DMA pair. Keep the
                 * intent pending and require another stable-clock window before
                 * retrying the bounded resync. */
                s_bridge_bclk_qualifying = false;
                s_diag_bclk_qualifying = false;
                s_diag_bridge_resync_failures++;
            }
        }
    } else if (s_modem_voice_transport_available && audio_bridge_active()) {
        /* Hardware backstop if core0 misses a hangup URC. Only tear down after
         * this bridge actually saw BCLK, so a connect gap cannot false-trigger. */
        if (bclk_active) {
            s_bridge_saw_bclk = true;
        } else if (s_bridge_saw_bclk) {
            audio_bridge_stop();
            (void)modem_i2s_hal_stop();
            s_bridge_saw_bclk = false;
            s_diag_bridge_bclk_losses++;
            s_diag_bridge_last_stop_ms = now_ms;
        }
    } else {
        s_bridge_start_pending = false;
        s_bridge_bclk_qualifying = false;
        s_diag_bridge_start_pending = false;
        s_diag_bclk_qualifying = false;
        s_bridge_saw_bclk = false;
        (void)modem_i2s_hal_stop();
    }

    audio_update_active_flag();
}

void audio_service_get_diag(audio_service_diag_t *out) {
    if (out == NULL) {
        return;
    }
    out->voice_transport_available = s_modem_voice_transport_available;
    out->voice_transport_running = modem_i2s_hal_running();
    out->bridge_start_pending = s_diag_bridge_start_pending;
    out->bclk_qualifying = s_diag_bclk_qualifying;
    out->bridge_active = audio_bridge_active();
    out->bridge_start_requests = s_diag_bridge_start_requests;
    out->bridge_activations = s_diag_bridge_activations;
    out->bridge_stop_requests = s_diag_bridge_stop_requests;
    out->bridge_bclk_losses = s_diag_bridge_bclk_losses;
    out->bridge_resync_failures = s_diag_bridge_resync_failures;
    out->bridge_last_acquire_ms = s_diag_bridge_last_acquire_ms;
    out->bridge_max_acquire_ms = s_diag_bridge_max_acquire_ms;
    out->bridge_last_request_ms = s_diag_bridge_last_request_ms;
    out->bridge_last_active_ms = s_diag_bridge_last_active_ms;
    out->bridge_last_stop_ms = s_diag_bridge_last_stop_ms;
}

/* Debug/bench earpiece digital-gain trim (Q8: 256 = unity). Scales the final
 * codec-earpiece sample so loudness can be tuned by ear against the real 3210
 * before baking the value into the amplitude table. Set via the `digain` console
 * command; unity by default = no change. Read in audio_fill (core1 IRQ), written
 * here (core1 main loop) -- both core1, single aligned 32-bit access. */
static volatile int32_t s_earpiece_gain_q8 = 256;
/* Core1 owns these runtime values. Their defaults are the PodMic calibration
 * against the v6.00 handset; `keycal` can still override them in RAM for
 * repeatable bench comparisons without changing stored settings. */
static volatile uint16_t s_click_gain_q8 = AUDIO_KEYPAD_CLICK_GAIN_Q8_DEFAULT;
static volatile int16_t s_click_h2_q12 = AUDIO_KEYPAD_CLICK_H2_Q12_DEFAULT;
static volatile uint32_t s_click_h2_phase = AUDIO_KEYPAD_CLICK_H2_PHASE_Q32_DEFAULT;
static volatile int16_t s_click_h3_q12 = AUDIO_KEYPAD_CLICK_H3_Q12_DEFAULT;
static volatile uint32_t s_click_h3_phase = AUDIO_KEYPAD_CLICK_H3_PHASE_Q32_DEFAULT;
static volatile int16_t s_click_h4_q12 = AUDIO_KEYPAD_CLICK_H4_Q12_DEFAULT;
static volatile uint32_t s_click_h4_phase = AUDIO_KEYPAD_CLICK_H4_PHASE_Q32_DEFAULT;
static volatile int16_t s_click_h5_q12 = AUDIO_KEYPAD_CLICK_H5_Q12_DEFAULT;
static volatile uint32_t s_click_h5_phase = AUDIO_KEYPAD_CLICK_H5_PHASE_Q32_DEFAULT;
static volatile uint32_t s_click_attack_samples = AUDIO_CLICK_ATTACK_SAMPLES_DEFAULT;
static volatile uint32_t s_click_release_samples = AUDIO_CLICK_RELEASE_SAMPLES_DEFAULT;
static volatile uint16_t s_dtmf_gain_q8 = AUDIO_KEYPAD_DTMF_GAIN_Q8_DEFAULT;
static volatile uint8_t s_dtmf_low_weight = AUDIO_KEYPAD_DTMF_LOW_WEIGHT_DEFAULT;
static volatile uint8_t s_dtmf_high_weight = AUDIO_KEYPAD_DTMF_HIGH_WEIGHT_DEFAULT;
static volatile uint8_t s_dtmf_headset_low_weight =
    AUDIO_KEYPAD_DTMF_HEADSET_LOW_WEIGHT_DEFAULT;
static volatile uint8_t s_dtmf_headset_high_weight =
    AUDIO_KEYPAD_DTMF_HEADSET_HIGH_WEIGHT_DEFAULT;

bool audio_service_command_uses_buzzer(uint16_t command, uint16_t arg) {
    if (audio_arg_level(arg) == AUDIO_LEVEL_SILENT) {
        return false;
    }
    switch (command) {
    case CORE1_CMD_AUDIO_RINGTONE_PREVIEW:
    case CORE1_CMD_AUDIO_RINGTONE_LOOP:
    case CORE1_CMD_AUDIO_RINGTONE_MENU_PREVIEW:
    case CORE1_CMD_AUDIO_DEBUG_BUZZER_TEST:
    case CORE1_CMD_AUDIO_PACMAN_TONE:
    case CORE1_CMD_AUDIO_COMPOSER_PACKED:
    case CORE1_CMD_AUDIO_COMPOSER_PACKED_LOOP:
        return true;
    case CORE1_CMD_AUDIO_SYSTEM_TONE:
    case CORE1_CMD_AUDIO_TONES_SYSTEM_PREVIEW:
    case CORE1_CMD_AUDIO_SYSTEM_TONE_LOOP:
    case CORE1_CMD_AUDIO_SYSTEM_TONE_QUIET:
    case CORE1_CMD_AUDIO_SYSTEM_TONE_QUIET_LOOP:
        return system_tone_uses_buzzer(audio_arg_code(arg));
    default:
        return false;
    }
}

void audio_service_command(uint16_t command, uint16_t arg) {
    uint8_t code = audio_arg_code(arg);
    uint8_t level = audio_arg_level(arg);
    bool marker_vibra = audio_arg_marker_vibra(arg);
    char key = audio_key_char_from_id(code);
    if (command == CORE1_CMD_AUDIO_CLICK) {
        s_debug_buzzer_owned = false;
        start_click(level);
    } else if (command == CORE1_CMD_AUDIO_DTMF) {
        s_debug_buzzer_owned = false;
        if (find_dtmf(key) != 0) {
            start_dtmf(key, level);
        } else {
            start_click(level);
        }
    } else if (command == CORE1_CMD_AUDIO_STOP) {
        if (arg == 0u) {
            s_debug_buzzer_owned = false;
            s_debug_vibra_owned = false;
            stop_all_audio();
        } else {
            stop_key(key);
        }
    } else if (command == CORE1_CMD_AUDIO_TONES_CLICK_PREVIEW) {
        s_debug_buzzer_owned = false;
        start_click(level);
        s_tones_preview_owned = s_audio.kind == AUDIO_KIND_CLICK;
    } else if (command == CORE1_CMD_AUDIO_SYSTEM_TONE ||
               command == CORE1_CMD_AUDIO_TONES_SYSTEM_PREVIEW ||
               command == CORE1_CMD_AUDIO_SYSTEM_TONE_LOOP ||
               command == CORE1_CMD_AUDIO_SYSTEM_TONE_QUIET ||
               command == CORE1_CMD_AUDIO_SYSTEM_TONE_QUIET_LOOP) {
        s_debug_buzzer_owned = false;
        bool quiet = command == CORE1_CMD_AUDIO_SYSTEM_TONE_QUIET ||
                     command == CORE1_CMD_AUDIO_SYSTEM_TONE_QUIET_LOOP;
        int16_t amplitude = audio_level_amplitude(level);
        if (quiet && level != AUDIO_LEVEL_SILENT) {
            amplitude = AUDIO_QUIET_SYSTEM_TONE_AMPLITUDE;
        }
        if (command != CORE1_CMD_AUDIO_TONES_SYSTEM_PREVIEW) {
            s_tones_preview_owned = false;
        }
        start_system_tone(code, level,
                          command == CORE1_CMD_AUDIO_SYSTEM_TONE_LOOP ||
                              command == CORE1_CMD_AUDIO_SYSTEM_TONE_QUIET_LOOP,
                          marker_vibra, amplitude);
        if (command == CORE1_CMD_AUDIO_TONES_SYSTEM_PREVIEW) {
            s_tones_preview_owned = s_audio.kind == AUDIO_KIND_TONE_STREAM;
        }
    } else if (command == CORE1_CMD_AUDIO_RINGTONE_PREVIEW) {
        s_debug_buzzer_owned = false;
        s_tones_preview_owned = false;
        /* Live incoming Ring once -> buzzer (v6.00 mode 0xf0). The selected
         * Level 1..5 is the drive ceiling; mode 0xf2's special Level-5 preview
         * staging does not apply to this active-ring path. */
        start_ringtone(code, level, false, true, false, marker_vibra);
    } else if (command == CORE1_CMD_AUDIO_RINGTONE_LOOP) {
        s_debug_buzzer_owned = false;
        s_tones_preview_owned = false;
        /* Live incoming Ringing / Ascending -> buzzer (v6.00 mode 0xf0). Keep
         * the configured Level 1..5 unchanged; the Level-4-to-5 staging traced
         * below belongs specifically to mode 0xf2 volume preview. */
        start_ringtone(code, level, true, true, false, marker_vibra);
    } else if (command == CORE1_CMD_AUDIO_RINGTONE_MENU_PREVIEW) {
        s_debug_buzzer_owned = false;
        /* Tones-menu ringtone preview -> the magnetic buzzer, same transducer as
         * the live ring. Bench-confirmed on the original (earpiece removed):
         * ringtone previews are audible on the buzzer. The ROM's preview modes
         * 0xf1/0xf2 differ from the live-ring 0xf0 internally, but the physical
         * output is the buzzer for our product. At 0x00297da0, v6.00 gates the
         * Level-5 special case on mode 0xf2 and raw volume 10: it temporarily
         * uses raw level 9, then advances after timer 0x28. The mode-independent
         * v6.00 0x0a handler still executes light/vibra markers in previews. */
        start_ringtone(code, level, false, true, true, marker_vibra);
        s_tones_preview_owned = s_audio.kind == AUDIO_KIND_TONE_STREAM;
    } else if (command == CORE1_CMD_AUDIO_PACMAN_TONE) {
        s_debug_buzzer_owned = false;
        start_pacman_tone(code, level);
    } else if (command == CORE1_CMD_AUDIO_COMPOSER_NOTE) {
        s_debug_buzzer_owned = false;
        start_composer_note(code, level);
    } else if (command == CORE1_CMD_AUDIO_COMPOSER_PREVIEW_STOP) {
        stop_composer_preview();
    } else if (command == CORE1_CMD_AUDIO_COMPOSER_STOP) {
        stop_composer_audio();
    } else if (command == CORE1_CMD_AUDIO_TONES_PREVIEW_STOP) {
        if (s_tones_preview_owned) {
            stop_all_audio();
        }
    } else if (command == CORE1_CMD_AUDIO_VIBRA_PULSE ||
               command == CORE1_CMD_AUDIO_VIBRA_PULSE_LOOP) {
        s_debug_vibra_owned = false;
        start_vibra_pulse(arg, command == CORE1_CMD_AUDIO_VIBRA_PULSE_LOOP, time_ms());
    } else if (command == CORE1_CMD_AUDIO_DEBUG_BUZZER_TEST) {
        s_vibra_enabled = false;
        s_debug_buzzer_owned = true;
        s_tones_preview_owned = false;
        start_ringtone(code, level, false, true, true, false);
    } else if (command == CORE1_CMD_AUDIO_DEBUG_VIBRA_TEST) {
        s_debug_vibra_owned = true;
        start_vibra_pulse(arg, true, time_ms());
    } else if (command == CORE1_CMD_AUDIO_DEBUG_STOP) {
        if (s_debug_buzzer_owned) {
            stop_synth_audio();
            s_debug_buzzer_owned = false;
        }
        if (s_debug_vibra_owned) {
            stop_vibra_pulse();
            s_debug_vibra_owned = false;
        }
    } else if (command == CORE1_CMD_AUDIO_SET_EARPIECE_GAIN) {
        int32_t g = (int32_t)arg;
        s_earpiece_gain_q8 = g > 1024 ? 1024 : g; /* cap at 4x */
    } else if (command == CORE1_CMD_AUDIO_SET_CLICK_GAIN) {
        s_click_gain_q8 = arg > AUDIO_KEYPAD_GAIN_Q8_MAX
            ? AUDIO_KEYPAD_GAIN_Q8_MAX
            : arg;
    } else if (command == CORE1_CMD_AUDIO_SET_CLICK_H2 ||
               command == CORE1_CMD_AUDIO_SET_CLICK_H3 ||
               command == CORE1_CMD_AUDIO_SET_CLICK_H4 ||
               command == CORE1_CMD_AUDIO_SET_CLICK_H5) {
        int32_t harmonic = (int16_t)arg;
        if (harmonic > AUDIO_CLICK_HARMONIC_Q12_MAX) {
            harmonic = AUDIO_CLICK_HARMONIC_Q12_MAX;
        } else if (harmonic < -AUDIO_CLICK_HARMONIC_Q12_MAX) {
            harmonic = -AUDIO_CLICK_HARMONIC_Q12_MAX;
        }
        if (command == CORE1_CMD_AUDIO_SET_CLICK_H2) {
            s_click_h2_q12 = (int16_t)harmonic;
        } else if (command == CORE1_CMD_AUDIO_SET_CLICK_H3) {
            s_click_h3_q12 = (int16_t)harmonic;
        } else if (command == CORE1_CMD_AUDIO_SET_CLICK_H4) {
            s_click_h4_q12 = (int16_t)harmonic;
        } else {
            s_click_h5_q12 = (int16_t)harmonic;
        }
    } else if (command == CORE1_CMD_AUDIO_SET_CLICK_RELEASE_MS) {
        uint32_t release_ms = arg;
        if (release_ms > AUDIO_CLICK_RELEASE_MS_MAX) {
            release_ms = AUDIO_CLICK_RELEASE_MS_MAX;
        }
        s_click_release_samples = (AUDIO_SAMPLE_RATE * release_ms) / 1000u;
    } else if (command == CORE1_CMD_AUDIO_SET_CLICK_H2_PHASE ||
               command == CORE1_CMD_AUDIO_SET_CLICK_H3_PHASE ||
               command == CORE1_CMD_AUDIO_SET_CLICK_H4_PHASE ||
               command == CORE1_CMD_AUDIO_SET_CLICK_H5_PHASE) {
        int32_t degrees = (int16_t)arg;
        if (degrees > AUDIO_CLICK_HARMONIC_PHASE_DEG_MAX) {
            degrees = AUDIO_CLICK_HARMONIC_PHASE_DEG_MAX;
        } else if (degrees < -AUDIO_CLICK_HARMONIC_PHASE_DEG_MAX) {
            degrees = -AUDIO_CLICK_HARMONIC_PHASE_DEG_MAX;
        }
        uint32_t phase =
            (uint32_t)(((int64_t)degrees * 0x100000000ll) / 360);
        if (command == CORE1_CMD_AUDIO_SET_CLICK_H2_PHASE) {
            s_click_h2_phase = phase;
        } else if (command == CORE1_CMD_AUDIO_SET_CLICK_H3_PHASE) {
            s_click_h3_phase = phase;
        } else if (command == CORE1_CMD_AUDIO_SET_CLICK_H4_PHASE) {
            s_click_h4_phase = phase;
        } else {
            s_click_h5_phase = phase;
        }
    } else if (command == CORE1_CMD_AUDIO_SET_CLICK_ATTACK_MS) {
        uint32_t attack_ms = arg;
        if (attack_ms > AUDIO_CLICK_ATTACK_MS_MAX) {
            attack_ms = AUDIO_CLICK_ATTACK_MS_MAX;
        }
        s_click_attack_samples = (AUDIO_SAMPLE_RATE * attack_ms) / 1000u;
    } else if (command == CORE1_CMD_AUDIO_SET_DTMF_GAIN) {
        s_dtmf_gain_q8 = arg > AUDIO_KEYPAD_GAIN_Q8_MAX
            ? AUDIO_KEYPAD_GAIN_Q8_MAX
            : arg;
    } else if (command == CORE1_CMD_AUDIO_SET_DTMF_WEIGHTS ||
               command == CORE1_CMD_AUDIO_SET_DTMF_HEADSET_WEIGHTS) {
        uint8_t low = (uint8_t)(arg & 0xffu);
        uint8_t high = (uint8_t)(arg >> 8);
        if (low != 0u || high != 0u) {
            /* audio_fill snapshots these two bytes in the DMA IRQ. Publish the
             * pair atomically so a legal 1:0 -> 0:1 calibration transition can
             * never expose a transient 0:0 divisor. Both writers are core1. */
            uint32_t irq_state = save_and_disable_interrupts();
            if (command == CORE1_CMD_AUDIO_SET_DTMF_HEADSET_WEIGHTS) {
                s_dtmf_headset_low_weight = low;
                s_dtmf_headset_high_weight = high;
            } else {
                s_dtmf_low_weight = low;
                s_dtmf_high_weight = high;
            }
            restore_interrupts(irq_state);
        }
    } else if (command == CORE1_CMD_AUDIO_SET_BUZZER_DUTY) {
        uint8_t percent = arg == UINT16_MAX ? UINT8_MAX
            : (arg > 50u ? 50u : (uint8_t)arg);
        buzzer_hal_debug_set_duty_percent(percent);
    }
    audio_update_active_flag();
}

static void audio_fill(void *ctx, int16_t *dst, uint32_t frame_count) {
    audio_state_t *state = (audio_state_t *)ctx;
    /* Pull one downlink block from the modem bridge (mono; silence if not primed)
     * and mix it with the local tone synth per frame below. Dormant (no pull,
     * voice_frames=0) until audio_bridge_start() at call connect. */
    int16_t downlink[AUDIO_BRIDGE_BLOCK_FRAMES];
    uint32_t voice_frames = 0u;
    if (audio_bridge_active()) {
        voice_frames = frame_count < AUDIO_BRIDGE_BLOCK_FRAMES ? frame_count : AUDIO_BRIDGE_BLOCK_FRAMES;
        audio_bridge_downlink_pull(downlink, (uint16_t)voice_frames);
    }
    const bool right_invert = audio_bridge_right_invert();
    const bool headset_route = audio_bridge_route() == (uint8_t)NAU_ROUTE_HEADSET;
    for (uint32_t i = 0; i < frame_count; i++) {
        int32_t sample = 0;
        if (state->kind == AUDIO_KIND_CLICK) {
            if (state->sample_index >= AUDIO_CLICK_SAMPLES) {
                state->kind = AUDIO_KIND_SILENCE;
                s_tones_preview_owned = false;
            } else {
                int32_t env = click_envelope_q8(state->sample_index);
                int32_t wave = click_wave_q15(state->phase0);
                int64_t scaled = (int64_t)wave * state->amplitude *
                                 s_click_gain_q8 * env;
                int64_t divisor = (int64_t)32767 * 256 * 256;
                scaled /= divisor;
                sample = scaled > 32767 ? 32767 : (scaled < -32768 ? -32768 : (int32_t)scaled);
                state->phase0 += state->step0;
                state->sample_index++;
            }
        } else if (state->kind == AUDIO_KIND_DTMF) {
            /* Physical 3210 bench: a continuously held dial key stops sounding
             * at about 3.84 s even if the key remains down. Keep the key identity
             * only while the tone is live; its eventual KEY_UP then becomes a
             * harmless owner-scoped stop. */
            if (state->sample_index >= AUDIO_DTMF_MAX_SAMPLES) {
                state->kind = AUDIO_KIND_SILENCE;
                state->active_key = 0;
            } else {
                uint32_t low_weight = headset_route
                    ? s_dtmf_headset_low_weight
                    : s_dtmf_low_weight;
                uint32_t high_weight = headset_route
                    ? s_dtmf_headset_high_weight
                    : s_dtmf_high_weight;
                uint32_t weight_total = low_weight + high_weight;
                int64_t low = (int64_t)sine_q15(state->phase0) * low_weight;
                int64_t high = (int64_t)sine_q15(state->phase1) * high_weight;
                int64_t scaled = (low + high) * state->amplitude * s_dtmf_gain_q8;
                scaled /= (int64_t)32767 * weight_total * 256;
                sample = scaled > 32767 ? 32767 : (scaled < -32768 ? -32768 : (int32_t)scaled);
                state->phase0 += state->step0;
                state->phase1 += state->step1;
                state->sample_index++;
            }
        } else if (state->kind == AUDIO_KIND_TONE_STREAM) {
            while (state->tone_segment_remaining == 0u && state->kind == AUDIO_KIND_TONE_STREAM) {
                if (!tone_next_segment(state)) {
                    finish_tone_stream(state);
                    break;
                }
            }
            if (state->kind == AUDIO_KIND_TONE_STREAM && state->tone_segment_remaining > 0u) {
                /* Ringtone (square role) goes to the dedicated buzzer when one is
                 * fitted, so the I2S speaker stays silent for it; without a
                 * buzzer pin we fall back to a square voicing on the speaker so
                 * the ring is still audible during bring-up.
                 * Real-3210 bench (2026-07-10, HDC-5 on an original): with a
                 * headset inserted, buzzer-path sounds play on BOTH the buzzer
                 * AND the headset -- so on the headset route the I2S copy is not
                 * muted (square voicing to the HP pair while tone_next_segment
                 * keeps driving the physical buzzer). ROM dig: the original's
                 * mechanism is an ASIC tap of the buzzer source onto the
                 * accessory path (route bit 0x08, re-asserted by the alert
                 * marker handler 0x297a8c) gated on NOT-in-call -- so mid-call
                 * alerts stay off the ear there; mirror that by keeping the
                 * I2S copy muted while the voice bridge runs. [BP] verify the
                 * headset copy's level by ear (square voicing confirmed by the
                 * dig: the tap carries the buzzer waveform). */
                bool to_buzzer = state->sequence_square && buzzer_hal_available() &&
                                 (audio_bridge_route() != (uint8_t)NAU_ROUTE_HEADSET ||
                                  audio_bridge_active());
                if (!state->tone_rest && !to_buzzer) {
                    uint32_t elapsed = state->tone_segment_total - state->tone_segment_remaining;
                    uint32_t env = 256u;
                    if (AUDIO_TONE_ATTACK_SAMPLES > 0u && elapsed < AUDIO_TONE_ATTACK_SAMPLES) {
                        env = (elapsed * 256u) / AUDIO_TONE_ATTACK_SAMPLES;
                    }
                    if (AUDIO_TONE_RELEASE_SAMPLES > 0u &&
                        state->tone_segment_remaining < AUDIO_TONE_RELEASE_SAMPLES) {
                        uint32_t rel = (state->tone_segment_remaining * 256u) / AUDIO_TONE_RELEASE_SAMPLES;
                        if (rel < env) {
                            env = rel;
                        }
                    }
                    if (state->sequence_square) {
                        /* Buzzer fallback voicing (piezo/magnetic-style square). */
                        sample = (state->phase0 & 0x80000000u) ? -(int32_t)state->amplitude
                                                               : (int32_t)state->amplitude;
                        sample = (sample * (int32_t)env) / 256;
                    } else {
                        sample = (int32_t)(((int64_t)sine_q15(state->phase0) * state->amplitude * (int32_t)env) /
                                           (32767 * 256));
                    }
                    state->phase0 += state->step0;
                }
                state->tone_segment_remaining--;
                state->sample_index++;
            }
        } else if (state->kind == AUDIO_KIND_SEQUENCE || state->kind == AUDIO_KIND_COMPOSER_PACKED) {
            bool wrapped = false;
            while (state->tone_segment_remaining == 0u &&
                   (state->kind == AUDIO_KIND_SEQUENCE || state->kind == AUDIO_KIND_COMPOSER_PACKED)) {
                if (!sequence_next_segment(state)) {
                    if (state->kind == AUDIO_KIND_COMPOSER_PACKED && state->loop && !wrapped) {
                        /* Own-tone ring: replay the whole melody until stopped
                         * (1:1 with the original's repeat-forever ring). One wrap
                         * per fill -- sequence_next_segment always yields a >=1
                         * sample segment from a valid step (sequence_len >= 1), so
                         * this cannot spin. */
                        wrapped = true;
                        state->sequence_pos = 0u;
                        state->phase0 = 0u;
                        continue;
                    }
                    state->kind = AUDIO_KIND_SILENCE;
                    /* End of own-tone melody: never leave the buzzer driving the
                     * last note (the TONE_STREAM path does this via
                     * finish_tone_stream; the composer path ends here). */
                    if (state->sequence_square) {
                        buzzer_hal_off();
                    }
                    break;
                }
            }
            if ((state->kind == AUDIO_KIND_SEQUENCE || state->kind == AUDIO_KIND_COMPOSER_PACKED) &&
                state->tone_segment_remaining > 0u) {
                uint32_t elapsed = state->tone_segment_total - state->tone_segment_remaining;
                uint32_t env = 256u;
                if (AUDIO_TONE_ATTACK_SAMPLES > 0u && elapsed < AUDIO_TONE_ATTACK_SAMPLES) {
                    env = (elapsed * 256u) / AUDIO_TONE_ATTACK_SAMPLES;
                }
                if (AUDIO_TONE_RELEASE_SAMPLES > 0u &&
                    state->tone_segment_remaining < AUDIO_TONE_RELEASE_SAMPLES) {
                    uint32_t rel = (state->tone_segment_remaining * 256u) / AUDIO_TONE_RELEASE_SAMPLES;
                    if (rel < env) {
                        env = rel;
                    }
                }
                /* Own tone (square role) plays on the buzzer; keep the I2S sample
                 * silent for it (buzzer driven in sequence_next_segment). Without
                 * a buzzer pin, fall back to the square voicing on the speaker.
                 * Headset inserted: duplicate to the HP pair too (same real-3210
                 * both-transducers rule + not-in-call gate as the TONE_STREAM
                 * path above). */
                bool to_buzzer = state->sequence_square && buzzer_hal_available() &&
                                 (audio_bridge_route() != (uint8_t)NAU_ROUTE_HEADSET ||
                                  audio_bridge_active());
                if (state->step0 != 0u && !to_buzzer) {
                    if (state->sequence_square) {
                        sample = (state->phase0 & 0x80000000u) ? -(int32_t)state->amplitude : (int32_t)state->amplitude;
                        sample = (sample * (int32_t)env) / 256;
                    } else {
                        sample = (int32_t)(((int64_t)sine_q15(state->phase0) * state->amplitude * (int32_t)env) /
                                           (32767 * 256));
                    }
                    state->phase0 += state->step0;
                }
                state->tone_segment_remaining--;
                state->sample_index++;
            }
        } else if (state->kind == AUDIO_KIND_COMPOSER_NOTE) {
            uint32_t env = 256u;
            if (AUDIO_TONE_ATTACK_SAMPLES > 0u && state->sample_index < AUDIO_TONE_ATTACK_SAMPLES) {
                env = (state->sample_index * 256u) / AUDIO_TONE_ATTACK_SAMPLES;
            }
            sample = (int32_t)(((int64_t)sine_q15(state->phase0) * state->amplitude * (int32_t)env) /
                               (32767 * 256));
            state->phase0 += state->step0;
            state->sample_index++;
        }
        /* The codec output stages both sit at a neutral 0 dB baseline. Apply the
         * measured HDC-5 attenuation only to locally synthesized PCM, before
         * mixing, so Telit downlink remains untouched. During a call the user's
         * analog call-volume step then scales voice and local feedback together. */
        if (headset_route) {
            sample = headset_local_sample(sample);
        }
        int32_t mono = sample;
        if (i < voice_frames) {
            /* Saturating mix of downlink voice + local tone. Both sit at
             * conservative headroom; user volume is codec analog gain, not a
             * per-sample scale here. Local UI tones stay audible during a call. */
            int32_t mixed = (int32_t)downlink[i] + sample;
            if (mixed > 32767) {
                mixed = 32767;
            } else if (mixed < -32768) {
                mixed = -32768;
            }
            mono = mixed;
        }
        if (s_earpiece_gain_q8 != 256) {
            int32_t g = (mono * s_earpiece_gain_q8) >> 8; /* bench loudness trim */
            mono = g > 32767 ? 32767 : (g < -32768 ? -32768 : g);
        }
        /* BTL receiver/loudspeaker: NON-inverted mono into both slots (the single
         * BTL inversion lives in codec R43). Headset differential mono: invert
         * the right slot per the route (right_invert). Exactly one inversion. */
        dst[(i * 2u) + 0u] = (int16_t)mono;
        dst[(i * 2u) + 1u] = right_invert ? (int16_t)(-mono) : (int16_t)mono;
    }
}

static void start_click(uint8_t level) {
    int16_t amplitude = keypad_audio_amplitude(level);
    if (amplitude == 0) {
        stop_all_audio();
        return;
    }
    uint32_t irq_state = save_and_disable_interrupts();
    s_audio.kind = AUDIO_KIND_CLICK;
    s_audio.active_key = 0;
    s_audio.sample_index = 0;
    s_audio.phase0 = 0;
    s_audio.phase1 = 0;
    s_audio.step0 = phase_step(AUDIO_CLICK_HZ);
    s_audio.step1 = 0;
    s_audio.amplitude = amplitude;
    restore_interrupts(irq_state);
    finish_speaker_audio_start();
}

static void start_dtmf(char key, uint8_t level) {
    const dtmf_row_t *row = find_dtmf(key);
    if (row == 0) {
        start_click(level);
        return;
    }
    int16_t amplitude = keypad_audio_amplitude(level);
    if (amplitude == 0) {
        stop_all_audio();
        return;
    }

    uint32_t irq_state = save_and_disable_interrupts();
    s_audio.kind = AUDIO_KIND_DTMF;
    s_audio.active_key = key;
    s_audio.sample_index = 0;
    s_audio.phase0 = 0;
    s_audio.phase1 = 0;
    s_audio.step0 = phase_step(row->low_hz);
    s_audio.step1 = phase_step(row->high_hz);
    s_audio.amplitude = amplitude;
    restore_interrupts(irq_state);
    finish_speaker_audio_start();
}

/* Alert tones the original plays on the magnetic buzzer (not the codec note
 * path): alarm main (12) and the SMS Standard/Special/Ascending alerts
 * (29/30/32). SMS "Beep once" (31) and the alarm secondary beep (10) stay on
 * the codec path. Bench-confirmed on the original handset (earpiece removed,
 * buzzer installed): these are audible, beep-once is not. */
static bool system_tone_uses_buzzer(uint8_t index) {
    switch (index) {
    case 12u: /* alarm main */
    case 29u: /* SMS Standard */
    case 30u: /* SMS Special */
    case 32u: /* SMS Ascending */
    /* "Warning and game tones" (the category gated by the Warning/game-tones
     * profile setting): the games' navigation / limit / win / lose beeps.
     * Bench-confirmed on the ORIGINAL handset -- Snake's in-game and game-over
     * sounds play on the buzzer -- overturning the RE trace's "cobba_earpiece"
     * note (the same way rings/alarms/SMS turned out buzzer, not earpiece). None
     * of these collide with an earpiece-only tone (keypad/service = 10, ringback
     * = 28); index 8 is also note-dialog record 15's tone, itself a
     * warning-category tone that correctly follows to the buzzer. The buzzer duty
     * comes from start_tone_bytes seeding s_env_ceiling with the level. */
    case 3u:  /* nav / step */
    case 8u:  /* game upper-limit beep + "Battery empty" dialog (record 15) */
    case 9u:  /* lower-limit beep */
    case 16u: /* game over */
    case 18u:
    case 19u:
    case 20u: /* win / level */
    /* Battery LOW warning (dialog record 16, tone 7): owner bench = buzzer, like
     * Battery empty (8). Exclusive to that dialog, so the index flip is clean.
     * (Charger-insert tones 10/11 stay on the earpiece; "Battery full" would
     * too.) */
    case 7u:
    /* Error/warning feedback beep (tone 5): the clock wrong-digit warning
     * (owner-confirmed buzzer) AND the error note-dialogs that share it (records
     * 0/21/50). v6.00 routes by tone INDEX, not by caller, so one index = one
     * transducer -- the wrong-digit and those error dialogs all play tone 5 on
     * the buzzer. (The clock "Time not set"/"Invalid time" dialogs are record 2 =
     * tone 10, which stays on the earpiece -- owner-confirmed.) */
    case 5u:
        return true;
    default:
        return false;
    }
}

static void start_system_tone(uint8_t index, uint8_t level, bool loop,
                              bool marker_vibra, int16_t amplitude) {
    uint16_t len = 0;
    const uint8_t *data = system_tone_data(index, &len);
    if (index == 0u || data == 0 || len == 0u) {
        start_click(level);
        return;
    }
    /* Buzzer-class alerts use square voicing and honour their 0x09 crescendo.
     * Alarm 12 starts at level 1 and reaches level 5 in five audible stages;
     * other resources retain the ROM-derived generic seed. Standard/Special
     * (29/30) carry no seed and remain flat. */
    bool buzzer = system_tone_uses_buzzer(index);
    uint8_t env_seed = index == 12u ? AUDIO_ALARM_ENV_SEED_LEVEL : AUDIO_ENV_SEED_LEVEL;
    start_tone_bytes(data, len, level, amplitude,
                     loop ? AUDIO_TONE_REPEAT_FOREVER : 1u,
                     buzzer, buzzer, env_seed, marker_vibra);
}

/* Audio level (0..AUDIO_LEVEL_MAX) -> magnetic-buzzer drive-table index. The
 * index mapping is linear; buzzer_hal owns the measured, deliberately nonlinear
 * duty curve. */
static uint8_t audio_to_buzzer_level(uint8_t level) {
    if (level >= AUDIO_LEVEL_MAX) {
        return BUZZER_LEVEL_MAX;
    }
    return (uint8_t)((uint16_t)level * BUZZER_LEVEL_MAX / AUDIO_LEVEL_MAX);
}

static void start_ringtone(uint8_t index, uint8_t level, bool loop, bool to_buzzer,
                           bool dynamics, bool marker_vibra) {
    const ringtone_t *ringtone = ringtone_by_index(index);
    if (ringtone == 0 || ringtone->data == 0 || ringtone->length == 0u) {
        return;
    }
    /* to_buzzer -> the magnetic buzzer (square voicing at the note frequency),
     * the transducer for BOTH the live ring and the menu preview. `dynamics`
     * enables the 0x09 envelope. Controlled PodMic captures of v6.00 show
     * Levels 1..4 flat, while Level 5 plays the first Nokia Tune pass at Level 4
     * and the second at Level 5. The ringtone-specific seed below reproduces
     * that mode-0xf2 preview boundary; live mode 0xf0 still receives the selected
     * level but does not enter this preview-only envelope. */
    start_tone_bytes(ringtone->data,
                     ringtone->length,
                     level,
                     audio_level_amplitude(level),
                     loop ? AUDIO_TONE_REPEAT_FOREVER : 2u,
                     to_buzzer,
                     dynamics,
                     AUDIO_RINGTONE_ENV_SEED_LEVEL,
                     marker_vibra);
}

static void start_pacman_tone(uint8_t index, uint8_t level) {
    const audio_sequence_step_t *sequence = PACMAN_DOT0;
    uint8_t len = (uint8_t)(sizeof(PACMAN_DOT0) / sizeof(PACMAN_DOT0[0]));
    if (index == 1u) {
        sequence = PACMAN_DOT1;
        len = (uint8_t)(sizeof(PACMAN_DOT1) / sizeof(PACMAN_DOT1[0]));
    } else if (index == 2u) {
        sequence = PACMAN_POWER;
        len = (uint8_t)(sizeof(PACMAN_POWER) / sizeof(PACMAN_POWER[0]));
    } else if (index == 3u) {
        sequence = PACMAN_GHOST;
        len = (uint8_t)(sizeof(PACMAN_GHOST) / sizeof(PACMAN_GHOST[0]));
    } else if (index == 4u) {
        sequence = PACMAN_DEATH;
        len = (uint8_t)(sizeof(PACMAN_DEATH) / sizeof(PACMAN_DEATH[0]));
    } else if (index == 5u) {
        sequence = PACMAN_WIN;
        len = (uint8_t)(sizeof(PACMAN_WIN) / sizeof(PACMAN_WIN[0]));
    }
    int16_t amplitude = audio_level_amplitude(level);
    if (amplitude == 0 || sequence == 0 || len == 0u) {
        stop_all_audio();
        return;
    }

    uint32_t irq_state = save_and_disable_interrupts();
    s_audio.kind = AUDIO_KIND_SEQUENCE;
    s_audio.active_key = 0;
    s_audio.sample_index = 0;
    s_audio.phase0 = 0;
    s_audio.phase1 = 0;
    s_audio.step0 = 0;
    s_audio.step1 = 0;
    s_audio.sequence = sequence;
    s_audio.sequence_len = len;
    s_audio.sequence_pos = 0u;
    s_audio.sequence_square = true;
    /* square role -> buzzer: seed the buzzer-duty ceiling from THIS tone's level.
     * sequence_next_segment() drives buzzer_hal_set_level(audio_to_buzzer_level(
     * s_env_ceiling)); without this it would inherit a stale global (0 at boot ->
     * silent game tones on buzzer hardware). No crescendo for game blips. */
    s_env_active = false;
    s_env_ceiling = level > AUDIO_LEVEL_MAX ? AUDIO_LEVEL_MAX : level;
    s_audio.tone_segment_total = 0u;
    s_audio.tone_segment_remaining = 0u;
    s_audio.amplitude = amplitude;
    restore_interrupts(irq_state);
    finish_speaker_audio_start();
}

void audio_service_start_composer_packed(const uint8_t *data, uint16_t len, uint8_t level) {
    s_debug_buzzer_owned = false;
    s_tones_preview_owned = false;
    start_composer_packed(data, len, level, false);
    audio_update_active_flag();
}

void audio_service_start_composer_packed_loop(const uint8_t *data, uint16_t len, uint8_t level) {
    s_debug_buzzer_owned = false;
    s_tones_preview_owned = false;
    start_composer_packed(data, len, level, true);
    audio_update_active_flag();
}

static void start_composer_note(uint8_t pitch, uint8_t level) {
    uint16_t hz = pitch == 0x40u ? 0u : tone_frequency_hz(pitch);
    if (hz == 0u) {
        stop_all_audio();
        return;
    }
    int16_t amplitude = audio_level_amplitude(level);
    if (amplitude == 0) {
        stop_all_audio();
        return;
    }

    uint32_t irq_state = save_and_disable_interrupts();
    s_audio.kind = AUDIO_KIND_COMPOSER_NOTE;
    s_audio.active_key = 0;
    s_audio.sample_index = 0;
    s_audio.phase0 = 0;
    s_audio.phase1 = 0;
    s_audio.step0 = phase_step(hz);
    s_audio.step1 = 0;
    s_audio.amplitude = amplitude;
    restore_interrupts(irq_state);
    finish_speaker_audio_start();
}

static void start_composer_packed(const uint8_t *data, uint16_t len, uint8_t level, bool loop) {
    int16_t amplitude = audio_level_amplitude(level);
    if (amplitude == 0 || data == 0 || len == 0u) {
        stop_all_audio();
        return;
    }

    audio_sequence_step_t steps[96];
    uint8_t step_count = 0u;
    if (!composer_packed_decode(data, len, steps, (uint8_t)(sizeof(steps) / sizeof(steps[0])), &step_count) ||
        step_count == 0u) {
        stop_all_audio();
        return;
    }

    uint32_t irq_state = save_and_disable_interrupts();
    memcpy(s_audio.composer_sequence, steps, step_count * sizeof(steps[0]));
    s_audio.kind = AUDIO_KIND_COMPOSER_PACKED;
    s_audio.active_key = 0;
    s_audio.sample_index = 0;
    s_audio.phase0 = 0;
    s_audio.phase1 = 0;
    s_audio.step0 = 0;
    s_audio.step1 = 0;
    s_audio.sequence = s_audio.composer_sequence;
    s_audio.sequence_len = step_count;
    s_audio.sequence_pos = 0u;
    /* Own tone IS a ringtone: square voicing on the magnetic buzzer, like the
     * PPM ringtones -- not the codec/earpiece. Flat (composer data carries no
     * 0x09 crescendo); the ring volume sets the buzzer duty via s_env_ceiling. */
    s_audio.sequence_square = true;
    s_env_active = false;
    s_env_ceiling = level > AUDIO_LEVEL_MAX ? AUDIO_LEVEL_MAX : level;
    s_audio.loop = loop;
    s_audio.tone_segment_total = 0u;
    s_audio.tone_segment_remaining = 0u;
    s_audio.amplitude = amplitude;
    restore_interrupts(irq_state);
    finish_speaker_audio_start();
}

static void start_tone_bytes(const uint8_t *data, uint16_t len, uint8_t level,
                             int16_t amplitude, uint8_t repeats, bool square,
                             bool dynamics, uint8_t env_seed_level,
                             bool marker_vibra) {
    if (data == 0 || len == 0u) {
        stop_all_audio();
        return;
    }
    if (amplitude == 0) {
        stop_all_audio();
        return;
    }
    if (s_debug_vibra_owned) {
        stop_vibra_pulse();
        s_debug_vibra_owned = false;
    }

    uint32_t irq_state = save_and_disable_interrupts();
    s_audio.kind = AUDIO_KIND_TONE_STREAM;
    /* Voicing role: ringtone melodies use the harsh square voicing of the
     * original's piezo buzzer; earpiece/COBBA system tones stay clean sine. */
    s_audio.sequence_square = square;
    s_audio.active_key = 0;
    s_audio.sample_index = 0;
    s_audio.phase0 = 0;
    s_audio.phase1 = 0;
    s_audio.step0 = 0;
    s_audio.step1 = 0;
    s_audio.tone_data = data;
    s_audio.tone_len = len;
    s_audio.tone_start = tone_start_offset(data, len);
    s_audio.tone_pos = s_audio.tone_start;
    s_audio.tone_repeat_depth = 0u;
    s_audio.tone_restart_count = repeats == 0u ? 1u : repeats;
    s_audio.tone_segment_total = 0u;
    s_audio.tone_segment_remaining = 0u;
    s_audio.tone_rest = 1u;
    s_audio.dynamics = dynamics; /* earpiece ringtone path opts in; others stay flat */
    s_audio.amplitude = amplitude;
    /* Replace stream + marker ownership in one IRQ-protected publication. The
     * previous stream can finish either before this block or after it becomes
     * the current stream, never between an arm command and this start. */
    s_vibra_enabled = marker_vibra;
    if (marker_vibra) {
        vibra_hal_set_strength(AUDIO_VIBRA_STRENGTH_STOCK);
    }
    if (!s_vibra_pulse_active) {
        vibra_hal_set(false);
    }
    /* Set the envelope/light state under the same lock so the audio IRQ can't
     * process the stream's 0x09 header before s_audio.dynamics/s_env_ceiling land. */
    s_env_active = false; /* armed only by a 0x09 seed in a dynamics-enabled stream */
    s_env_ceiling = level > AUDIO_LEVEL_MAX ? AUDIO_LEVEL_MAX : level;
    s_env_seed_level = env_seed_level > s_env_ceiling ? s_env_ceiling : env_seed_level;
    /* A 0x09 crescendo seed in the HEADER (before tone_start, e.g. alarm 12 /
     * SMS Ascending 32) is skipped by the note stepper, so arm the ramp here --
     * once, at start. Unlike the in-stream 0x09 (re-read every loop), this ramps
     * from the seed to the ceiling a single time across the looping alert. */
    if (dynamics && s_env_ceiling > s_env_seed_level) {
        for (uint16_t i = 0u; i < s_audio.tone_start && i < len; i++) {
            if (data[i] == 0x09u) {
                s_env_level = s_env_seed_level;
                s_env_next_step_ms = time_ms() + AUDIO_ENV_STEP_MS;
                s_env_active = true;
                s_audio.amplitude = audio_level_amplitude(s_env_level);
                break;
            }
        }
    }
    s_ring_light = false;
    restore_interrupts(irq_state);
    if (!square) {
        /* A sine stream still owns the marker gate published above. Only
         * silence a buzzer path displaced by this codec-speaker stream; the
         * generic non-stream cleanup would incorrectly clear the new gate. */
        buzzer_hal_off();
    }
}

static void stop_key(char key) {
    uint32_t irq_state = save_and_disable_interrupts();
    if (s_audio.kind == AUDIO_KIND_DTMF && s_audio.active_key == key) {
        s_audio.kind = AUDIO_KIND_SILENCE;
        s_audio.active_key = 0;
    }
    restore_interrupts(irq_state);
}

static void stop_composer_preview(void) {
    uint32_t irq_state = save_and_disable_interrupts();
    if (s_audio.kind == AUDIO_KIND_COMPOSER_NOTE) {
        s_audio.kind = AUDIO_KIND_SILENCE;
        s_audio.active_key = 0;
    }
    restore_interrupts(irq_state);
}

static void stop_composer_audio(void) {
    bool stopped = false;
    uint32_t irq_state = save_and_disable_interrupts();
    if (s_audio.kind == AUDIO_KIND_COMPOSER_NOTE ||
        s_audio.kind == AUDIO_KIND_COMPOSER_PACKED) {
        s_audio.kind = AUDIO_KIND_SILENCE;
        s_audio.active_key = 0;
        s_audio.loop = false;
        stopped = true;
    }
    restore_interrupts(irq_state);
    if (stopped) {
        buzzer_hal_off();
        s_env_active = false;
        s_ring_light = false;
    }
}

static void stop_synth_audio(void) {
    uint32_t irq_state = save_and_disable_interrupts();
    s_audio.kind = AUDIO_KIND_SILENCE;
    s_audio.active_key = 0;
    s_audio.loop = false;
    restore_interrupts(irq_state);
    buzzer_hal_off();     /* never leave the ring buzzer driving */
    s_env_active = false; /* M5: drop the crescendo ramp */
    s_ring_light = false; /* M7: drop the ring-light flash */
    s_tones_preview_owned = false;
}

static void stop_all_audio(void) {
    stop_synth_audio();
    stop_vibra_pulse();
    s_vibra_enabled = false;
    vibra_hal_set(false); /* never leave the motor latched on after a stop */
    s_debug_buzzer_owned = false;
    s_debug_vibra_owned = false;
    s_tones_preview_owned = false;
}

static void finish_tone_stream(audio_state_t *state) {
    state->kind = AUDIO_KIND_SILENCE;
    s_debug_buzzer_owned = false;
    s_tones_preview_owned = false;
    /* Clear s_vibra_enabled so it defaults FALSE after any stream ends: several
     * marker-bearing tones (the message-alert system tones 28+, which embed
     * 0x0a 0x01 vibra markers) carry a disabled marker gate, so leaving the flag
     * set after a one-shot ring/preview would make an unrelated SMS tone vibrate
     * non-deterministically. Each new stream
     * now publishes its own marker gate atomically in start_tone_bytes(). */
    s_vibra_enabled = false;
    if (!s_vibra_pulse_active) {
        vibra_hal_set(false); /* bytecode streams may end after a vibra-on marker */
    }
    buzzer_hal_off();     /* silence the ring buzzer at end of melody */
    /* If a ring stream ends abnormally (malformed bytecode exiting the
     * REPEAT_FOREVER loop) the crescendo/light state must not linger. */
    s_env_active = false; /* M5: drop the crescendo ramp */
    s_ring_light = false; /* M7: drop the ring-light flash */
}

static void finish_speaker_audio_start(void) {
    /* Any ordinary speaker-path start replaces a Tones picker preview. The
     * dedicated Tones click command reclaims ownership after this helper. */
    s_tones_preview_owned = false;
    buzzer_hal_off();
    /* A non-stream sound (keypad click / DTMF / composer note) preempts any
     * marker-driven tone stream -- e.g. a melody ring -- which OWNS the vibra and
     * ring-light markers. Those markers are level-latches (0x0a on/off edges) with
     * no further off-edge once the driving stream is replaced, so drop them here
     * too (mirrors stop_all_audio / finish_tone_stream); otherwise the motor runs
     * continuously and the ring light stays lit until the next AUDIO_STOP. The
     * separate s_vibra_pulse "Own tone" path does not route through here.) */
    s_vibra_enabled = false;
    if (!s_vibra_pulse_active) {
        vibra_hal_set(false);
    }
    s_ring_light = false;
}

static void start_vibra_pulse(uint16_t arg, bool loop, uint32_t now_ms) {
    s_vibra_pulse_strength = vibra_arg_strength(arg);
    s_vibra_pulse_ticks = vibra_arg_ticks(arg);
    s_vibra_pulse_loop = loop;
    s_vibra_pulse_active = true;
    s_vibra_pulse_until_ms = now_ms + (uint32_t)s_vibra_pulse_ticks * AUDIO_VIBRA_TICK_MS;
    s_vibra_pulse_next_ms = s_vibra_pulse_until_ms;
    vibra_hal_set_strength(s_vibra_pulse_strength);
    vibra_hal_set(true);
}

static void stop_vibra_pulse(void) {
    s_vibra_pulse_active = false;
    s_vibra_pulse_loop = false;
    s_vibra_pulse_until_ms = 0u;
    s_vibra_pulse_next_ms = 0u;
    vibra_hal_set(false);
}

static uint8_t vibra_arg_strength(uint16_t arg) {
    uint8_t strength = (uint8_t)(arg >> 8u);
    return strength == 0u ? VIBRA_HAL_STRENGTH_STOCK : strength;
}

static uint8_t vibra_arg_ticks(uint16_t arg) {
    uint8_t ticks = (uint8_t)(arg & 0xffu);
    return ticks == 0u ? AUDIO_VIBRA_PULSE_DEFAULT_TICKS : ticks;
}

static const dtmf_row_t *find_dtmf(char key) {
    for (uint32_t i = 0; i < sizeof(DTMF_TABLE) / sizeof(DTMF_TABLE[0]); i++) {
        if (DTMF_TABLE[i].key == key) {
            return &DTMF_TABLE[i];
        }
    }
    return 0;
}

static uint32_t phase_step(uint16_t hz) {
    return (uint32_t)(((uint64_t)hz << 32) / AUDIO_SAMPLE_RATE);
}

static int16_t sine_q15(uint32_t phase) {
    uint8_t index = (uint8_t)(phase >> 24);
    uint8_t quadrant = index >> 6;
    uint8_t offset = index & 0x3fu;
    int16_t value;
    switch (quadrant) {
    case 0:
        value = SINE_Q15[offset];
        break;
    case 1:
        value = SINE_Q15[64u - offset];
        break;
    case 2:
        value = (int16_t)-SINE_Q15[offset];
        break;
    default:
        value = (int16_t)-SINE_Q15[64u - offset];
        break;
    }
    return value;
}

static int32_t click_wave_q15(uint32_t phase) {
    int32_t h2_q12 = s_click_h2_q12;
    int32_t h3_q12 = s_click_h3_q12;
    int32_t h4_q12 = s_click_h4_q12;
    int32_t h5_q12 = s_click_h5_q12;
    int32_t norm_q12 = 4096 +
                       (h2_q12 < 0 ? -h2_q12 : h2_q12) +
                       (h3_q12 < 0 ? -h3_q12 : h3_q12) +
                       (h4_q12 < 0 ? -h4_q12 : h4_q12) +
                       (h5_q12 < 0 ? -h5_q12 : h5_q12);
    int64_t shaped = (int64_t)sine_q15(phase) * 4096 +
                     (int64_t)sine_q15(phase * 2u + s_click_h2_phase) * h2_q12 +
                     (int64_t)sine_q15(phase * 3u + s_click_h3_phase) * h3_q12 +
                     (int64_t)sine_q15(phase * 4u + s_click_h4_phase) * h4_q12 +
                     (int64_t)sine_q15(phase * 5u + s_click_h5_phase) * h5_q12;
    return (int32_t)(shaped / norm_q12);
}

static int32_t click_envelope_q8(uint32_t sample_index) {
    int32_t envelope = 256;
    uint32_t attack = s_click_attack_samples;
    if (attack != 0u && sample_index < attack) {
        envelope = (int32_t)((sample_index * 256u) / attack);
    }
    uint32_t release = s_click_release_samples;
    uint32_t release_start = AUDIO_CLICK_SAMPLES - release;
    if (release != 0u && sample_index >= release_start) {
        uint32_t remaining = AUDIO_CLICK_SAMPLES - sample_index;
        int32_t release_envelope = (int32_t)((remaining * 256u) / release);
        if (release_envelope < envelope) {
            envelope = release_envelope;
        }
    }
    return envelope;
}

static uint16_t tone_start_offset(const uint8_t *data, uint16_t len) {
    if (len >= 5u && data[0] == 0x00u && data[1] == 0x00u && data[2] == 0x02u &&
        data[3] >= 0xfcu) {
        return 5u;
    }
    if (len >= 3u && data[0] == 0x00u && data[1] == 0x02u && data[2] >= 0xfcu) {
        return len >= 4u && data[3] == 0x09u ? 4u : 3u;
    }
    if (len >= 3u && data[0] == 0x00u && (data[1] == 0x01u || data[1] == 0x09u) &&
        data[2] >= 0xfcu) {
        return 3u;
    }
    if (len >= 3u && data[0] == 0x00u && (data[1] == 0x01u || data[1] == 0x09u)) {
        return 2u;
    }
    if (len >= 2u && data[0] == 0x00u &&
        (data[1] == 0x40u || (data[1] >= 0x41u && data[1] <= 0xa4u) ||
         data[1] == 0xa6u || data[1] == 0x05u || data[1] == 0x06u ||
         data[1] == 0x0au)) {
        return 1u;
    }
    return 0u;
}

static bool tone_next_segment(audio_state_t *state) {
    uint8_t guard = 0u;
    while (guard++ < 128u && state->tone_data != 0 && state->tone_pos < state->tone_len) {
        uint8_t cmd = state->tone_data[state->tone_pos++];
        if (cmd == 0x0bu) {
            return false;
        }
        if (cmd == 0x07u && state->tone_pos < state->tone_len && state->tone_data[state->tone_pos] == 0x0bu) {
            state->tone_pos++;
            if (state->tone_restart_count == AUDIO_TONE_REPEAT_FOREVER) {
                state->tone_pos = state->tone_start;
                state->tone_repeat_depth = 0u;
                continue;
            }
            if (state->tone_restart_count > 1u) {
                state->tone_restart_count--;
                state->tone_pos = state->tone_start;
                state->tone_repeat_depth = 0u;
                continue;
            }
            return false;
        }
        if (cmd == 0x05u) {
            if (state->tone_pos >= state->tone_len) {
                return false;
            }
            uint8_t repeat = state->tone_data[state->tone_pos++];
            if (state->tone_repeat_depth < AUDIO_TONE_REPEAT_DEPTH) {
                tone_repeat_t *slot = &state->tone_repeats[state->tone_repeat_depth++];
                slot->start_pos = state->tone_pos;
                slot->remaining = repeat == 0u ? 1u : repeat;
            }
            continue;
        }
        if (cmd == 0x06u) {
            if (state->tone_repeat_depth > 0u) {
                tone_repeat_t *slot = &state->tone_repeats[state->tone_repeat_depth - 1u];
                if (slot->remaining > 1u) {
                    slot->remaining--;
                    state->tone_pos = slot->start_pos;
                } else {
                    state->tone_repeat_depth--;
                }
            }
            continue;
        }
        if (cmd == 0x0au) {
            /* Secondary control marker. Operand 0x01 = lights + vibra on, 0x08 =
             * vibra only, {0x0b,0xf7,0xfe} = both off; other operands (e.g. the
             * common 0x0a 0x0a tone-control/loop-sync marker) are neither.
             * M7: lights (s_ring_light, mirrored to the backlight by core0) are NOT
             * gated by the vibra setting -- the original flashes the keypad/display
             * lights on every ring regardless of Vibrating-alert. Vibra stays gated
             * by s_vibra_enabled. */
            uint8_t operand = 0u;
            if (state->tone_pos < state->tone_len) {
                operand = state->tone_data[state->tone_pos++];
            }
            if (operand == 0x01u) {
                s_ring_light = true;
            } else if (operand == 0x0bu || operand == 0xf7u || operand == 0xfeu) {
                s_ring_light = false;
            }
            if (s_vibra_enabled) {
                if (operand == 0x01u || operand == 0x08u) {
                    vibra_hal_set(true);
                } else if (operand == 0x0bu || operand == 0xf7u || operand == 0xfeu) {
                    vibra_hal_set(false);
                }
            }
            continue;
        }
        if (cmd == 0x01u || cmd == 0x02u || cmd == 0x08u || cmd == 0x0eu) {
            if (state->tone_pos < state->tone_len) {
                state->tone_pos++;
            }
            continue;
        }
        if (cmd == 0x09u) {
            /* Volume seed. The stream-specific seed starts the ~4.1 s step ramp;
             * s_audio.dynamics gates it. No crescendo when the configured ceiling
             * is at/below the seed. */
            if (s_audio.dynamics && s_env_ceiling > s_env_seed_level) {
                s_env_level = s_env_seed_level;
                s_env_next_step_ms = time_ms() + AUDIO_ENV_STEP_MS;
                s_env_active = true;
                s_audio.amplitude = audio_level_amplitude(s_env_level);
            }
            continue;
        }
        if (cmd == 0x0cu || cmd == 0x0du || cmd == 0x0fu || cmd == 0x10u) {
            continue;
        }
        if (cmd == 0x04u) {
            /* M14: keyed-tone / DTMF-in-resource command (orig 3-byte form
             * 04 <key> <dur>; key 0xff = "runtime key" supplied at play time).
             * v6.00 emits a dual-tone DTMF burst here, but our DTMF uses the
             * dedicated start_dtmf path and no played stream embeds 0x04 -- so we
             * consume the full instruction and emit a rest of <dur>, keeping stream
             * timing correct. This prevents 0x04 from being mis-read as a pitch byte
             * (which would cascade-corrupt the rest of the stream). Decoding it to
             * actually play DTMF in-stream is a documented non-port (not reachable). */
            if (state->tone_pos < state->tone_len) {
                state->tone_pos++; /* key byte (literal or 0xff runtime) -- unused */
            }
            uint8_t dtmf_dur = 0u;
            if (state->tone_pos < state->tone_len) {
                dtmf_dur = state->tone_data[state->tone_pos++];
            }
            state->tone_segment_total = tone_duration_samples(dtmf_dur);
            state->tone_segment_remaining = state->tone_segment_total;
            state->tone_rest = 1u;
            state->step0 = 0u;
            return true;
        }
        if (state->tone_pos >= state->tone_len) {
            return false;
        }

        uint8_t duration = state->tone_data[state->tone_pos++];
        /* M2: 0x40 = rest; 0xa6 is the original's dynamic-pitch note = 900 Hz (ROM
         * freq word 0x0384). tone_frequency_hz() returns 0 for pitch > 0xa4, which
         * would render 0xa6 as silence -- so resolve it explicitly (e.g.
         * system_tone_04 '00 a6 23 0b' must sound at 900 Hz, not a ~280 ms gap). */
        uint16_t hz;
        if (cmd == 0x40u) {
            hz = 0u;
        } else if (cmd == 0xa6u) {
            hz = 900u;
        } else {
            hz = tone_frequency_hz(cmd);
        }
        state->tone_segment_total = tone_duration_samples(duration);
        state->tone_segment_remaining = state->tone_segment_total;
        state->tone_rest = hz == 0u ? 1u : 0u;
        state->step0 = hz == 0u ? 0u : phase_step(hz);
        /* Ringtone streams (square role) drive the dedicated magnetic buzzer at
         * the note frequency; the I2S path stays silent for them. */
        if (state->sequence_square && buzzer_hal_available()) {
            buzzer_hal_set_freq(hz);
            /* Ring volume / crescendo -> buzzer duty: the live crescendo level
             * while ramping, else the configured ceiling (= the ring volume). */
            buzzer_hal_set_level(audio_to_buzzer_level(s_env_active ? s_env_level : s_env_ceiling));
        }
        return true;
    }
    return false;
}

static bool sequence_next_segment(audio_state_t *state) {
    if (state->sequence == 0 || state->sequence_pos >= state->sequence_len) {
        return false;
    }
    const audio_sequence_step_t *step = &state->sequence[state->sequence_pos++];
    state->tone_segment_total = (uint32_t)(((uint64_t)AUDIO_SAMPLE_RATE * step->duration_ms) / 1000u);
    if (state->tone_segment_total == 0u) {
        state->tone_segment_total = 1u;
    }
    state->tone_segment_remaining = state->tone_segment_total;
    state->phase0 = 0u;
    state->step0 = phase_step(step->hz);
    /* Own-tone (square role) drives the magnetic buzzer at the note frequency,
     * like the PPM ringtones; hz==0 rests silence it. The I2S path stays quiet
     * (gated below). */
    if (state->sequence_square && buzzer_hal_available()) {
        buzzer_hal_set_freq(step->hz);
        buzzer_hal_set_level(audio_to_buzzer_level(s_env_ceiling));
    }
    return true;
}

static uint32_t tone_duration_samples(uint8_t units) {
    uint32_t samples = (uint32_t)((((uint64_t)units * 6400000ull) + 25097ull) / 50195ull);
    return samples == 0u ? 1u : samples;
}

static int16_t audio_level_amplitude(uint8_t level) {
    /* [BP] Earpiece loudness, tuned by ear against a real 3210 on board #1
     * (2026-07-03): the old table topped out at 20000/32767 (~61% FS), leaving the
     * earpiece noticeably quieter than the original. Scaled ~x1.6 so level 5 lands
     * near full scale (~32000, the loudest clean point before clipping) with the
     * lower steps proportional. Keypad clicks and DTMF apply their measured
     * three-step trim separately; alarms, ringtones, and system tones continue
     * to consume this table unchanged. Re-confirm by ear if the enclosure or
     * transducer changes; the `digain` console knob re-derives it live. */
    static const int16_t amplitudes[AUDIO_LEVEL_MAX + 1u] = {
        0,
        5600,
        9000,
        14400,
        22400,
        32000,
    };
    if (level > AUDIO_LEVEL_MAX) {
        level = AUDIO_LEVEL_MAX;
    }
    return amplitudes[level];
}

static int16_t keypad_audio_amplitude(uint8_t level) {
    int32_t amplitude = audio_level_amplitude(level);
    uint16_t gain_q8 = audio_keypad_level_gain_q8(level);
    return (int16_t)((amplitude * gain_q8 + 128) / 256);
}

static int32_t headset_local_sample(int32_t sample) {
    int64_t scaled = (int64_t)sample * AUDIO_HEADSET_LOCAL_GAIN_Q15;
    scaled += scaled >= 0 ? (1ll << 14) : -(1ll << 14);
    return (int32_t)(scaled / (1ll << 15));
}

static bool composer_packed_decode(const uint8_t *data,
                                   uint16_t len,
                                   audio_sequence_step_t *steps,
                                   uint8_t step_cap,
                                   uint8_t *out_count) {
    if (data == 0 || len == 0u || steps == 0 || step_cap == 0u || out_count == 0) {
        return false;
    }
    *out_count = 0u;
    /* Static, not stack: this runs on core 1's fixed 4 KiB stack behind the
     * single serialized command dispatcher (never from the audio IRQ), and a
     * 384-byte transient on top of the caller's steps[] copy is budget the
     * IRQ frames want more than we do. Sized to the caller's step buffer. */
    static composer_note_event_t events[96];
    uint8_t event_cap = step_cap < (uint8_t)(sizeof(events) / sizeof(events[0]))
                            ? step_cap
                            : (uint8_t)(sizeof(events) / sizeof(events[0]));
    uint8_t tempo = 0u;
    uint8_t count = 0u;
    if (!composer_codec_decode(data, len, &tempo, events, event_cap, &count)) {
        return false;
    }
    for (uint8_t i = 0u; i < count; i++) {
        steps[i].hz = composer_packed_pitch_hz(events[i].octave, events[i].pitch_code);
        steps[i].duration_ms = composer_packed_duration_ms(
            tempo, events[i].duration_code, events[i].dotted ? 1u : 0u);
    }
    *out_count = count;
    return count != 0u;
}

static uint16_t composer_packed_duration_ms(uint8_t tempo, uint8_t duration_code, uint8_t dotted) {
    return composer_note_ms(composer_tempo_bpm(tempo),
                            composer_duration_denominator(duration_code),
                            dotted == 1u);
}

static uint16_t composer_packed_pitch_hz(uint8_t octave, uint8_t pitch_code) {
    if (pitch_code == 0u || pitch_code > 12u) {
        return 0u;
    }
    static const uint8_t semitone_by_pitch[] = {
        0u, 0u, 1u, 2u, 3u, 4u, 5u, 6u, 7u, 8u, 9u, 10u, 11u,
    };
    if (octave < 1u || octave > 3u) {
        octave = 1u;
    }
    uint8_t freq_index = (uint8_t)(62u + semitone_by_pitch[pitch_code] + (octave - 1u) * 12u);
    uint8_t pitch = (uint8_t)(0x40u + freq_index);
    return tone_frequency_hz(pitch);
}
