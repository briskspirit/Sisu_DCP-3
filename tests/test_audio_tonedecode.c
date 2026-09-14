/* Host unit tests for the tone-bytecode decoder in src/audio/audio_service.c.
 *
 * Target: tone_next_segment() and its helpers (tone_duration_samples,
 * tone_start_offset, plus the opcode cases including the new 0x04 DTMF-rest,
 * 0x09 crescendo seed, 0x0a vibra/light markers, 0xa6=900Hz, 0x40=rest, the
 * 0x05/0x06 repeat block and the 0x07 0x0b / 0x0b end markers).
 *
 * audio_service.c is file-static and pulls in pico/hardware + many project
 * headers. We make it host-buildable by:
 *   - providing a host stub for <hardware/sync.h> under tests/stubs (already on
 *     the runner include path) so the save_and_disable_interrupts()/
 *     restore_interrupts() critical sections become no-ops (single-threaded);
 *   - directly #including the .c so the file-static decoder is visible;
 *   - linking the active generated tones_data.c for tone_frequency_hz /
 *     ringtone_by_index / system_tone_data. Original-asset runs exercise the
 *     ROM-derived tables; public runs use structurally equivalent synthetic
 *     resources;
 *   - linking the real audio-level policy and stubbing the remaining HAL /
 *     bridge / timebase leaves (they have no bearing on the decoder's bounds
 *     logic, but the linker demands them).
 *
 * ASan/UBSan are the primary bug detectors: the decoder must never read past
 * tone_len for any truncated / malformed / oversized bytecode. We also assert
 * decoder invariants (tone_pos in-bounds, durations >= 1, rest/step coherence)
 * derived from the opcode semantics, not from observed output.
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "hal/board.h"      /* AUDIO_SAMPLE_RATE, pins */
#include "audio/audio_levels.h"
#include "audio/composer_codec.h"
#include "generated/tones.h"

/* ------------------------------------------------------------------ stubs --
 * Leaf dependencies audio_service.c calls but which live in other TUs we do
 * not link. None affect the decoder's pointer/bounds arithmetic. The
 * vibra/buzzer/ring-light stubs record their last state so the 0x0a marker and
 * buzzer-frequency cases can be observed where useful. */

static bool s_stub_vibra_on;
static uint8_t s_stub_vibra_strength;
static bool s_stub_buzzer_avail;          /* default false: speaker fallback path */
static uint16_t s_stub_buzzer_freq;
static bool s_stub_buzzer_freq_set;
static uint32_t s_stub_now_ms;

void vibra_hal_set(bool on) { s_stub_vibra_on = on; }
void vibra_hal_set_strength(uint8_t s) { s_stub_vibra_strength = s; }

static uint8_t s_stub_buzzer_level = 0xffu;
static uint8_t s_stub_buzzer_duty_percent = UINT8_MAX;
bool buzzer_hal_available(void) { return s_stub_buzzer_avail; }
void buzzer_hal_off(void) { s_stub_buzzer_freq = 0u; }
void buzzer_hal_set_freq(uint16_t hz) { s_stub_buzzer_freq = hz; s_stub_buzzer_freq_set = true; }
void buzzer_hal_set_level(uint8_t level) { s_stub_buzzer_level = level; }
void buzzer_hal_debug_set_duty_percent(uint8_t percent) {
    s_stub_buzzer_duty_percent = percent;
}

static bool s_stub_bridge_active; /* headset-dup in-call gate tests toggle this */
static int16_t s_stub_downlink_sample;
static uint32_t s_stub_bridge_start_count;
static uint32_t s_stub_bridge_stop_count;
bool audio_bridge_active(void) { return s_stub_bridge_active; }
void audio_bridge_init(void) {}
void audio_bridge_start(void) {
    s_stub_bridge_active = true;
    s_stub_bridge_start_count++;
}
void audio_bridge_stop(void) {
    s_stub_bridge_active = false;
    s_stub_bridge_stop_count++;
}
bool audio_bridge_right_invert(void) { return false; }
static uint8_t s_stub_bridge_route; /* 0 = NAU_ROUTE_HANDSET; tests may set HEADSET */
uint8_t audio_bridge_route(void) { return s_stub_bridge_route; }
void audio_bridge_downlink_pull(int16_t *dst, uint16_t count) {
    for (uint16_t i = 0u; i < count; i++) {
        dst[i] = s_stub_downlink_sample;
    }
}

#include "audio/audio_i2s_hal.h"
bool audio_i2s_hal_init(audio_i2s_fill_fn_t fill, void *ctx) { (void)fill; (void)ctx; return true; }
static uint32_t s_stub_modem_init_count;
static bool s_stub_modem_running;
static bool s_stub_modem_start_ok = true;
static bool s_stub_modem_stop_ok = true;
static uint32_t s_stub_modem_start_count;
static uint32_t s_stub_modem_stop_count;
bool modem_i2s_hal_init(void) {
    s_stub_modem_init_count++;
    return true;
}
bool modem_i2s_hal_start(void) {
    s_stub_modem_start_count++;
    if (s_stub_modem_start_ok) {
        s_stub_modem_running = true;
    }
    return s_stub_modem_start_ok;
}
bool modem_i2s_hal_stop(void) {
    s_stub_modem_stop_count++;
    if (s_stub_modem_stop_ok) {
        s_stub_modem_running = false;
    }
    return s_stub_modem_stop_ok;
}
bool modem_i2s_hal_running(void) { return s_stub_modem_running; }
static bool s_stub_bclk_active;
static bool s_stub_bclk_fresh;
static uint32_t s_stub_bclk_poll_count;
static uint32_t s_stub_bclk_reset_count;
static uint32_t s_stub_modem_resync_count;
static bool s_stub_modem_resync_ok = true;
bool modem_i2s_hal_bclk_poll(bool *fresh_edges) {
    s_stub_bclk_poll_count++;
    if (fresh_edges != 0) {
        *fresh_edges = s_stub_bclk_fresh;
    }
    return s_stub_bclk_active;
}
void modem_i2s_hal_bclk_reset(void) {
    s_stub_bclk_active = false;
    s_stub_bclk_fresh = false;
    s_stub_bclk_reset_count++;
}
bool modem_i2s_hal_resync(void) {
    s_stub_modem_resync_count++;
    return s_stub_modem_resync_ok;
}

uint32_t time_ms(void) { return s_stub_now_ms; }
int32_t time_diff_ms(uint32_t a, uint32_t b) { return (int32_t)(a - b); }

/* log.h's LOGI/LOGE expand to log_write(); audio_service_init() references it
 * even though no decoder test calls init. Swallow it. */
#include "services/log.h"
void log_write(log_level_t level, const char *tag, const char *fmt, ...) {
    (void)level; (void)tag; (void)fmt;
}

/* Pull in the unit under test (file-static decoder now visible). */
#include "../src/audio/audio_service.c"

/* ----------------------------------------------------------------- harness */
static int s_failures;

static void assert_true(bool condition, const char *message) {
    if (!condition) {
        fprintf(stderr, "FAIL: %s\n", message);
        s_failures++;
    }
}

static void test_disabled_modem_voice_transport_is_inert(void) {
    s_stub_modem_init_count = 0u;
    s_stub_modem_running = false;
    s_stub_modem_start_count = 0u;
    s_stub_modem_stop_count = 0u;
    s_stub_bclk_poll_count = 0u;
    s_stub_bclk_reset_count = 0u;
    s_stub_bridge_active = false;
    s_audio_active = false;
    s_bridge_start_pending = false;
    s_bridge_bclk_qualifying = false;
    s_bridge_saw_bclk = false;

    audio_service_init(false);
    assert_true(s_stub_modem_init_count == 0u,
                "disabled voice transport does not initialize modem I2S");

    audio_service_bridge_start();
    assert_true(!s_bridge_start_pending,
                "disabled voice transport rejects bridge-start intent");
    assert_true(s_stub_bclk_reset_count == 0u,
                "disabled voice transport does not reset modem BCLK");
    assert_true(s_stub_modem_start_count == 0u,
                "disabled voice transport does not start modem I2S");

    audio_service_tick(1u);
    assert_true(s_stub_bclk_poll_count == 0u,
                "disabled voice transport does not poll modem BCLK");
    assert_true(!audio_service_is_active(),
                "rejected bridge intent does not keep the audio core active");

    /* Leave the shared unit-under-test enabled for the bridge qualification
     * cases that follow. */
    audio_service_init(true);
    assert_true(s_stub_modem_init_count == 1u,
                "enabled voice transport initializes modem I2S once");
}

static void test_bridge_transport_failure_is_fail_closed(void) {
    s_stub_bridge_active = false;
    s_bridge_start_pending = false;
    s_bridge_bclk_qualifying = false;
    s_stub_modem_running = false;
    s_stub_modem_start_count = 0u;
    s_stub_modem_stop_count = 0u;
    s_stub_modem_start_ok = false;
    s_stub_modem_stop_ok = true;

    audio_service_bridge_start();
    assert_true(!s_bridge_start_pending,
                "transport-start failure does not publish pending bridge");
    assert_true(!audio_service_is_active(),
                "transport-start failure does not hold audio active");

    s_stub_modem_start_ok = true;
    audio_service_bridge_start();
    assert_true(s_bridge_start_pending && s_stub_modem_running,
                "transport can start after a transient failure");
    s_stub_modem_stop_ok = false;
    audio_service_bridge_stop();
    assert_true(s_stub_modem_running,
                "failed transport stop remains visibly running");
    assert_true(audio_service_is_active(),
                "failed transport stop blocks low-power entry");
    s_stub_modem_stop_ok = true;
    audio_service_tick(1u);
    assert_true(!s_stub_modem_running,
                "idle tick retries and completes transport stop");
}

/* Bridge start is intentionally qualified: the call-state edge keeps core1
 * awake, but audio stays muted until this session produces continuously fresh
 * modem BCLK for the full settle interval. */
static void test_bridge_start_waits_for_fresh_bclk(void) {
    memset(&s_audio, 0, sizeof(s_audio));
    s_audio.kind = AUDIO_KIND_SILENCE;
    s_vibra_pulse_active = false;
    s_vibra_pulse_loop = false;
    s_env_active = false;
    s_stub_bridge_active = false;
    s_stub_bridge_start_count = 0u;
    s_stub_bridge_stop_count = 0u;
    s_stub_bclk_active = true; /* stale evidence must be cleared by start */
    s_stub_bclk_fresh = true;
    s_stub_bclk_reset_count = 0u;
    s_stub_modem_resync_count = 0u;
    s_stub_modem_resync_ok = true;
    s_stub_modem_start_ok = true;
    s_stub_modem_stop_ok = true;
    s_stub_modem_running = false;
    s_stub_modem_start_count = 0u;
    s_stub_modem_stop_count = 0u;
    s_bridge_saw_bclk = false;
    s_bridge_start_pending = false;
    s_bridge_bclk_qualifying = false;
    s_audio_active = false;

    audio_service_bridge_start();
    assert_true(s_bridge_start_pending, "bridge start waits for fresh BCLK");
    assert_true(s_stub_modem_running, "bridge start arms modem I2S transport");
    assert_true(s_stub_modem_start_count == 1u, "bridge start arms transport once");
    assert_true(!s_stub_bridge_active, "pending bridge remains muted");
    assert_true(s_stub_bclk_reset_count == 1u, "bridge start resets BCLK epoch");
    assert_true(s_stub_modem_resync_count == 0u, "bridge start does not resync before BCLK");
    assert_true(s_stub_bridge_start_count == 0u, "bridge start not published before BCLK");
    assert_true(audio_service_is_active(), "pending bridge keeps core1 service active");

    audio_service_tick(1u);
    assert_true(s_bridge_start_pending, "bridge remains pending without BCLK");
    assert_true(!s_stub_bridge_active, "bridge remains muted without BCLK");
    assert_true(s_stub_modem_resync_count == 0u, "no-BCLK tick does not resync");

    s_stub_bclk_active = true;
    s_stub_bclk_fresh = true;
    audio_service_tick(2u);
    assert_true(s_bridge_start_pending, "first fresh BCLK begins qualification");
    assert_true(s_bridge_bclk_qualifying, "BCLK qualification is armed");
    assert_true(!s_stub_bridge_active, "first fresh BCLK does not start bridge");

    s_stub_bclk_fresh = false;
    audio_service_tick(3u);
    assert_true(!s_bridge_bclk_qualifying, "missing fresh BCLK resets qualification");

    s_stub_bclk_fresh = true;
    audio_service_tick(4u);
    audio_service_tick(4u + AUDIO_BRIDGE_BCLK_STABLE_MS - 1u);
    assert_true(s_bridge_start_pending, "bridge waits for full stable-BCLK interval");
    assert_true(s_stub_modem_resync_count == 0u, "qualification does not resync early");

    audio_service_tick(4u + AUDIO_BRIDGE_BCLK_STABLE_MS);
    assert_true(!s_bridge_start_pending, "stable BCLK completes pending start");
    assert_true(s_stub_bridge_active, "stable BCLK publishes active bridge");
    assert_true(s_bridge_saw_bclk, "stable BCLK is recorded for loss detection");
    assert_true(s_stub_modem_resync_count == 1u, "stable BCLK triggers one resync");
    assert_true(s_stub_bridge_start_count == 1u, "stable BCLK starts bridge once");

    audio_service_tick(5u + AUDIO_BRIDGE_BCLK_STABLE_MS);
    assert_true(s_stub_modem_resync_count == 1u, "steady BCLK does not resync again");
    assert_true(s_stub_bridge_start_count == 1u, "steady BCLK does not restart bridge");

    audio_service_bridge_stop();
    assert_true(!s_stub_modem_running, "bridge stop parks modem I2S transport");
    audio_service_bridge_start();
    assert_true(s_bridge_start_pending, "second start enters pending state");
    uint32_t resyncs_before_cancel = s_stub_modem_resync_count;
    uint32_t starts_before_cancel = s_stub_bridge_start_count;
    audio_service_bridge_stop();
    assert_true(!s_stub_modem_running, "pending-start cancellation parks modem I2S");
    s_stub_bclk_active = true;
    s_stub_bclk_fresh = true;
    audio_service_tick(30u);
    assert_true(!s_bridge_start_pending, "stop cancels pending bridge start");
    assert_true(!s_stub_bridge_active, "cancelled bridge remains stopped");
    assert_true(s_stub_modem_resync_count == resyncs_before_cancel,
                "cancelled start cannot resync on later BCLK");
    assert_true(s_stub_bridge_start_count == starts_before_cancel,
                "cancelled start cannot publish on later BCLK");
}

static void test_bridge_start_retries_after_resync_failure(void) {
    memset(&s_audio, 0, sizeof(s_audio));
    s_audio.kind = AUDIO_KIND_SILENCE;
    s_stub_bridge_active = false;
    s_stub_bridge_start_count = 0u;
    s_stub_bclk_active = false;
    s_stub_bclk_fresh = false;
    s_stub_modem_resync_count = 0u;
    s_stub_modem_resync_ok = false;
    s_bridge_saw_bclk = false;
    s_bridge_start_pending = false;
    s_bridge_bclk_qualifying = false;

    audio_service_bridge_start();
    s_stub_bclk_active = true;
    s_stub_bclk_fresh = true;
    audio_service_tick(100u);
    audio_service_tick(100u + AUDIO_BRIDGE_BCLK_STABLE_MS);

    assert_true(s_stub_modem_resync_count == 1u, "failed resync is attempted once");
    assert_true(s_bridge_start_pending, "failed resync keeps bridge start pending");
    assert_true(!s_bridge_bclk_qualifying, "failed resync resets BCLK qualification");
    assert_true(!s_stub_bridge_active, "failed resync cannot publish active bridge");
    assert_true(s_stub_bridge_start_count == 0u, "failed resync cannot start mixer");

    s_stub_modem_resync_ok = true;
    audio_service_tick(121u);
    assert_true(s_stub_modem_resync_count == 1u,
                "resync retry requires a new stable-BCLK interval");
    audio_service_tick(121u + AUDIO_BRIDGE_BCLK_STABLE_MS);

    assert_true(s_stub_modem_resync_count == 2u, "safe resync retries after qualification");
    assert_true(!s_bridge_start_pending, "successful retry completes bridge start");
    assert_true(s_stub_bridge_active, "successful retry publishes active bridge");
    assert_true(s_stub_bridge_start_count == 1u, "successful retry starts mixer once");

    audio_service_bridge_stop();
    s_stub_modem_resync_ok = true;
}

/* Build a fresh, zeroed tone-stream state pointed at `data`, as start_tone_bytes
 * would leave it just before the first tone_next_segment() call. tone_start is
 * computed by the real tone_start_offset() so header skipping matches firmware.
 * dynamics/square mirror the two real callers (system tone vs earpiece ring). */
static void init_tone_state(audio_state_t *st,
                            const uint8_t *data,
                            uint16_t len,
                            uint8_t repeats,
                            bool square,
                            bool dynamics) {
    memset(st, 0, sizeof(*st));
    st->kind = AUDIO_KIND_TONE_STREAM;
    st->tone_data = data;
    st->tone_len = len;
    st->tone_start = tone_start_offset(data, len);
    st->tone_pos = st->tone_start;
    st->tone_repeat_depth = 0u;
    st->tone_restart_count = repeats == 0u ? 1u : repeats;
    st->tone_rest = 1u;
    st->sequence_square = square;
    st->dynamics = dynamics;
    st->amplitude = 20000;
}

/* Assert the post-conditions that must hold after EVERY tone_next_segment call,
 * regardless of opcode: the read cursor is within [0, tone_len], and a returned
 * segment has a sane (>=1) duration with rest/step coherence. */
static void check_segment_invariants(const audio_state_t *st, bool produced, const char *ctx) {
    char msg[96];
    snprintf(msg, sizeof(msg), "%s: tone_pos <= tone_len", ctx);
    assert_true(st->tone_pos <= st->tone_len, msg);
    if (produced) {
        snprintf(msg, sizeof(msg), "%s: segment duration >= 1", ctx);
        assert_true(st->tone_segment_total >= 1u && st->tone_segment_remaining >= 1u, msg);
        snprintf(msg, sizeof(msg), "%s: remaining <= total", ctx);
        assert_true(st->tone_segment_remaining <= st->tone_segment_total, msg);
        snprintf(msg, sizeof(msg), "%s: rest implies step0==0", ctx);
        /* A rest segment must carry no oscillator step; a voiced one must. */
        if (st->tone_rest) {
            assert_true(st->step0 == 0u, msg);
        }
    }
}

/* Drive the decoder to exhaustion (until it returns false), bounded so a
 * malformed loop cannot hang the test. Returns the number of segments produced.
 * Every iteration checks the universal invariants -- ASan independently catches
 * any OOB read inside tone_next_segment. */
static uint32_t drain_decoder(audio_state_t *st, const char *ctx) {
    uint32_t produced = 0u;
    for (uint32_t i = 0u; i < 100000u; i++) {
        bool ok = tone_next_segment(st);
        check_segment_invariants(st, ok, ctx);
        if (!ok) {
            return produced;
        }
        produced++;
        /* Consume the segment so a self-resetting (repeat-forever) stream keeps
         * advancing rather than handing back the same samples. */
        st->tone_segment_remaining = 0u;
    }
    /* Reaching the cap is itself a finding (decoder failed to terminate). */
    char msg[96];
    snprintf(msg, sizeof(msg), "%s: decoder terminated within 100000 segments", ctx);
    assert_true(false, msg);
    return produced;
}

/* =========================================================================
 * tone_duration_samples: hand-computed oracle from the ROM scale
 *   samples = (units * 6400000 + 25097) / 50195, clamped to >= 1.
 * ========================================================================= */
static void test_duration_samples_oracle(void) {
    assert_true(tone_duration_samples(0u) == 1u, "dur(0) clamps to 1");
    assert_true(tone_duration_samples(1u) == 128u, "dur(1) = 128 samples (8ms @16k)");
    /* units=0x23=35: (35*6400000 + 25097)/50195 = 224025097/50195 = 4463
     * (50195*4463 = 224020285 <= 224025097 < 50195*4464). */
    assert_true(tone_duration_samples(0x23u) == 4463u, "dur(35) = 4463 samples");
    /* units=255: 1632025097/50195 = 32513 */
    assert_true(tone_duration_samples(255u) == 32513u, "dur(255) = 32513 samples");
    /* Monotonic non-decreasing and never zero across the whole byte range. */
    uint32_t prev = 0u;
    for (uint32_t u = 0u; u <= 255u; u++) {
        uint32_t s = tone_duration_samples((uint8_t)u);
        assert_true(s >= 1u, "dur never zero");
        if (u > 0u) {
            assert_true(s >= prev, "dur monotonic non-decreasing");
        }
        prev = s;
    }
}

/* =========================================================================
 * 0x40 rest note: hz resolves to 0 -> tone_rest set, step0 zero. The duration
 * operand follows the opcode. Stream: "00 40 23 0b".  header skip -> pos at 0x40.
 * ========================================================================= */
static void test_rest_note_0x40(void) {
    static const uint8_t stream[] = {0x00u, 0x40u, 0x23u, 0x0bu};
    audio_state_t st;
    init_tone_state(&st, stream, sizeof(stream), 1u, false, false);
    /* tone_start_offset: data[0]==0x00, data[1]==0x40 -> returns 1. */
    assert_true(st.tone_start == 1u, "0x40 stream header skip = 1");
    bool ok = tone_next_segment(&st);
    assert_true(ok, "0x40 produces a segment");
    check_segment_invariants(&st, ok, "rest0x40");
    assert_true(st.tone_rest == 1u, "0x40 is a rest");
    assert_true(st.step0 == 0u, "0x40 rest has no oscillator step");
    assert_true(st.tone_segment_total == tone_duration_samples(0x23u), "0x40 rest duration from operand");
    /* Next: 0x0b end marker -> false. */
    assert_true(!tone_next_segment(&st), "0x0b ends the stream");
}

/* =========================================================================
 * 0xa6 dynamic-pitch note must resolve to 900 Hz (NOT silence, even though
 * tone_frequency_hz(0xa6)==0 because 0xa6 > 0xa4). Stream "00 a6 23 0b".
 * ========================================================================= */
static void test_note_0xa6_is_900hz(void) {
    assert_true(tone_frequency_hz(0xa6u) == 0u, "ROM table has no 0xa6 entry (>0xa4)");
    static const uint8_t stream[] = {0x00u, 0xa6u, 0x23u, 0x0bu};
    audio_state_t st;
    init_tone_state(&st, stream, sizeof(stream), 1u, false, false);
    assert_true(st.tone_start == 1u, "0xa6 stream header skip = 1");
    bool ok = tone_next_segment(&st);
    assert_true(ok, "0xa6 produces a segment");
    check_segment_invariants(&st, ok, "note0xa6");
    assert_true(st.tone_rest == 0u, "0xa6 is voiced, not a rest");
    assert_true(st.step0 == phase_step(900u), "0xa6 resolves to 900 Hz step");
}

/* A normal pitch (e.g. 0x40+offset within table) round-trips through
 * tone_frequency_hz and yields a voiced segment with the matching step. */
static void test_note_normal_pitch(void) {
    /* 0x4a is within [0x40,0xa4]; use whatever the ROM table says. */
    uint16_t hz = tone_frequency_hz(0x4au);
    assert_true(hz != 0u, "0x4a has a ROM frequency");
    static const uint8_t stream[] = {0x00u, 0x4au, 0x10u, 0x0bu};
    audio_state_t st;
    init_tone_state(&st, stream, sizeof(stream), 1u, false, false);
    bool ok = tone_next_segment(&st);
    assert_true(ok, "normal pitch produces a segment");
    check_segment_invariants(&st, ok, "noteNormal");
    assert_true(st.tone_rest == 0u, "normal pitch voiced");
    assert_true(st.step0 == phase_step(hz), "normal pitch step matches ROM hz");
    assert_true(st.tone_segment_total == tone_duration_samples(0x10u), "normal pitch duration");
}

/* =========================================================================
 * 0x04 DTMF-rest instruction (3-byte form: 04 <key> <dur>). It must consume key
 * + dur and emit a rest of <dur>, with step0 cleared, NOT misread the key as a
 * pitch. Stream "00 04 7f 23 0b" (header skip 0, since first byte is 0x00 and
 * second is 0x04, not in the start-offset table -> offset 0).
 * ========================================================================= */
static void test_dtmf_rest_0x04(void) {
    static const uint8_t stream[] = {0x00u, 0x04u, 0x7fu, 0x23u, 0x0bu};
    audio_state_t st;
    /* tone_start_offset: 0x04 is not a recognized header pitch -> offset 0.
     * So decode starts at byte 0 (0x00). 0x00 is handled as a default pitch
     * opcode (cmd=0x00): tone_frequency_hz(0)=0 -> rest, consumes the next byte
     * 0x04 as its duration. We instead want to land ON 0x04, so build a stream
     * that the decoder reaches 0x04 at: skip the leading 0x00 manually. */
    init_tone_state(&st, stream, sizeof(stream), 1u, false, false);
    st.tone_pos = 1u; /* point directly at the 0x04 opcode */
    bool ok = tone_next_segment(&st);
    assert_true(ok, "0x04 produces a (rest) segment");
    check_segment_invariants(&st, ok, "dtmf0x04");
    assert_true(st.tone_rest == 1u, "0x04 emits a rest");
    assert_true(st.step0 == 0u, "0x04 rest clears step0");
    assert_true(st.tone_segment_total == tone_duration_samples(0x23u),
                "0x04 duration taken from the dur operand, not the key");
    /* cursor advanced past key(0x7f) and dur(0x23): now at index 4 = 0x0b. */
    assert_true(st.tone_pos == 4u, "0x04 consumed key+dur (3-byte instruction)");
    assert_true(!tone_next_segment(&st), "stream ends after 0x04 instruction");
}

/* 0x04 at end-of-stream: only the opcode present, no key, no dur. Must not read
 * OOB; emits a rest with the default (0) duration -> clamped to 1 sample. */
static void test_dtmf_rest_0x04_truncated(void) {
    static const uint8_t stream[] = {0x04u};
    audio_state_t st;
    init_tone_state(&st, stream, sizeof(stream), 1u, false, false);
    st.tone_pos = 0u;
    bool ok = tone_next_segment(&st);
    /* Per code: 0x04 with no operands -> dtmf_dur stays 0 -> duration 1 sample. */
    assert_true(ok, "0x04-only still yields a (1-sample rest) segment");
    check_segment_invariants(&st, ok, "dtmf0x04trunc");
    assert_true(st.tone_rest == 1u, "0x04-only is a rest");
    assert_true(st.tone_segment_total == 1u, "0x04-only duration clamps to 1");
    assert_true(st.tone_pos == 1u, "0x04-only consumed just the opcode, no OOB");
}

/* 0x04 with only the key byte (no duration). Must consume the key, leave dur=0,
 * not read past the end. Stream "04 7f". */
static void test_dtmf_rest_0x04_key_only(void) {
    static const uint8_t stream[] = {0x04u, 0x7fu};
    audio_state_t st;
    init_tone_state(&st, stream, sizeof(stream), 1u, false, false);
    st.tone_pos = 0u;
    bool ok = tone_next_segment(&st);
    assert_true(ok, "0x04 key-only yields a segment");
    check_segment_invariants(&st, ok, "dtmf0x04keyonly");
    assert_true(st.tone_segment_total == 1u, "0x04 key-only dur clamps to 1");
    assert_true(st.tone_pos == 2u, "0x04 key-only consumed opcode+key, no OOB");
}

/* =========================================================================
 * 0x09 crescendo seed. With dynamics=true and a ceiling above the seed, it must
 * arm s_env_active and seed s_env_level. With dynamics=false it must NOT arm.
 * 0x09 produces no segment (continue), so it must be followed by a real note to
 * terminate cleanly.
 * ========================================================================= */
static void test_crescendo_seed_0x09_armed(void) {
    /* "00 09 fc ..." -> header offset table: data[0]==0,data[1]==0x09,data[2]>=0xfc
     * -> returns 3. To exercise 0x09 directly we point past the header at it. */
    static const uint8_t stream[] = {0x09u, 0x4au, 0x10u, 0x0bu};
    audio_state_t st;
    init_tone_state(&st, stream, sizeof(stream), 1u, false, /*dynamics=*/true);
    st.tone_pos = 0u;
    s_env_active = false;
    s_env_seed_level = AUDIO_ENV_SEED_LEVEL;
    s_env_ceiling = AUDIO_LEVEL_MAX;      /* well above the seed level (3) */
    s_audio.dynamics = true;              /* the seed case reads the global s_audio */
    s_audio.amplitude = 0;
    bool ok = tone_next_segment(&st);     /* processes 0x09 then the 0x4a note */
    check_segment_invariants(&st, ok, "cresc0x09");
    assert_true(ok, "0x09 seed followed by a note still produces the note");
    assert_true(s_env_active, "0x09 arms the crescendo when dynamics && ceiling>seed");
    assert_true(s_env_level == AUDIO_ENV_SEED_LEVEL, "0x09 seeds the level to SEED_LEVEL");
}

static void test_crescendo_seed_0x09_not_armed_without_dynamics(void) {
    static const uint8_t stream[] = {0x09u, 0x4au, 0x10u, 0x0bu};
    audio_state_t st;
    init_tone_state(&st, stream, sizeof(stream), 1u, true, /*dynamics=*/false);
    st.tone_pos = 0u;
    s_env_active = false;
    s_env_seed_level = AUDIO_ENV_SEED_LEVEL;
    s_audio.dynamics = false;             /* buzzer ring: no per-note level */
    s_env_ceiling = AUDIO_LEVEL_MAX;
    bool ok = tone_next_segment(&st);
    check_segment_invariants(&st, ok, "cresc0x09nodyn");
    assert_true(!s_env_active, "0x09 does NOT arm crescendo when dynamics is off");
}

/* Ceiling at/below the seed must not arm (low ring volumes start at the cap). */
static void test_crescendo_seed_0x09_low_ceiling(void) {
    static const uint8_t stream[] = {0x09u, 0x4au, 0x10u, 0x0bu};
    audio_state_t st;
    init_tone_state(&st, stream, sizeof(stream), 1u, false, true);
    st.tone_pos = 0u;
    s_env_active = false;
    s_env_seed_level = AUDIO_ENV_SEED_LEVEL;
    s_audio.dynamics = true;
    s_env_ceiling = AUDIO_ENV_SEED_LEVEL; /* == seed -> no headroom to ramp */
    bool ok = tone_next_segment(&st);
    (void)ok;
    assert_true(!s_env_active, "0x09 does not arm when ceiling <= seed");
}

static void test_alarm_crescendo_uses_five_low_to_full_levels(void) {
    bool saved_avail = s_stub_buzzer_avail;
    s_stub_buzzer_avail = true;
    s_stub_now_ms = 1000u;
    s_stub_buzzer_level = 0xffu;

    start_system_tone(12u, AUDIO_LEVEL_MAX, true, false,
                      audio_level_amplitude(AUDIO_LEVEL_MAX));
    assert_true(s_env_active, "alarm 12 arms its header crescendo");
    assert_true(s_env_seed_level == AUDIO_ALARM_ENV_SEED_LEVEL,
                "alarm 12 selects the low envelope seed");
    assert_true(s_env_level == 1u, "alarm starts at audio level 1");

    bool ok = tone_next_segment(&s_audio);
    check_segment_invariants(&s_audio, ok, "alarmCrescendo");
    assert_true(ok, "alarm stream reaches its first note");
    assert_true(s_stub_buzzer_level == 2u,
                "first alarm stage maps to low buzzer drive 2/10");

    static const uint8_t EXPECTED_BUZZER_LEVELS[] = {4u, 6u, 8u, 10u};
    for (size_t i = 0u; i < sizeof(EXPECTED_BUZZER_LEVELS); i++) {
        audio_service_tick(s_env_next_step_ms);
        assert_true(s_env_level == (uint8_t)(i + 2u),
                    "alarm envelope advances exactly one audio level");
        assert_true(s_stub_buzzer_level == EXPECTED_BUZZER_LEVELS[i],
                    "alarm buzzer follows the five-stage envelope");
    }
    audio_service_tick(s_env_next_step_ms);
    assert_true(s_env_level == AUDIO_LEVEL_MAX && s_stub_buzzer_level == BUZZER_LEVEL_MAX,
                "alarm envelope stops at full drive without overshoot");

    /* The alarm override must not silently retune the other 0x09 resource. */
    start_system_tone(32u, AUDIO_LEVEL_MAX, false, false,
                      audio_level_amplitude(AUDIO_LEVEL_MAX));
    assert_true(s_env_seed_level == AUDIO_ENV_SEED_LEVEL &&
                    s_env_level == AUDIO_ENV_SEED_LEVEL,
                "SMS Ascending retains the generic ROM-derived seed");

    stop_all_audio();
    s_stub_buzzer_avail = saved_avail;
}

static void test_ringtone_level5_preview_envelope_is_mode_scoped(void) {
    const uint16_t level4 = (uint16_t)(6u | (4u << 8u));
    const uint16_t level5 = (uint16_t)(6u | (5u << 8u));

    audio_service_command(CORE1_CMD_AUDIO_RINGTONE_MENU_PREVIEW, level4);
    assert_true(!s_env_active && s_env_ceiling == 4u,
                "ringtone Levels 1..4 remain flat at the selected volume");

    s_stub_now_ms = 2000u;
    audio_service_command(CORE1_CMD_AUDIO_RINGTONE_MENU_PREVIEW, level5);
    assert_true(s_env_active &&
                    s_env_seed_level == AUDIO_RINGTONE_ENV_SEED_LEVEL &&
                    s_env_level == 4u && s_env_ceiling == 5u,
                "Level-5 ringtone preview starts at stock Level 4");
    audio_service_tick(s_env_next_step_ms - 1u);
    assert_true(s_env_level == 4u,
                "Level-5 preview does not rise before the ROM timer interval");
    audio_service_tick(s_env_next_step_ms);
    assert_true(s_env_level == 5u,
                "Level-5 preview reaches full level in one stock step");

    audio_service_command(CORE1_CMD_AUDIO_RINGTONE_LOOP, level5);
    assert_true(!s_env_active && s_env_ceiling == 5u,
                "live Ringing uses configured Level 5 without preview staging");
    audio_service_command(CORE1_CMD_AUDIO_RINGTONE_PREVIEW, level5);
    assert_true(!s_env_active && s_env_ceiling == 5u,
                "live Ring once uses configured Level 5 without preview staging");
    assert_true(BUZZER_DUTY_AUDIO_LEVEL1_PERCENT == 2u &&
                    BUZZER_DUTY_AUDIO_LEVEL2_PERCENT == 3u &&
                    BUZZER_DUTY_AUDIO_LEVEL3_PERCENT == 5u &&
                    BUZZER_DUTY_AUDIO_LEVEL4_PERCENT == 8u &&
                    BUZZER_DUTY_AUDIO_LEVEL5_PERCENT == 24u,
                "ringtone levels retain the measured Rev B2 buzzer ladder");
    stop_all_audio();
}

/* =========================================================================
 * 0x0a control marker: operand 0x01 turns ring light on; 0x0b/0xf7/0xfe off.
 * Vibra fires only when s_vibra_enabled. 0x0a produces no segment.
 * ========================================================================= */
static void test_marker_0x0a_light_and_vibra(void) {
    /* light on (0x01) with vibra enabled, then a note. */
    static const uint8_t stream[] = {0x0au, 0x01u, 0x4au, 0x10u, 0x0bu};
    audio_state_t st;
    init_tone_state(&st, stream, sizeof(stream), 1u, false, false);
    st.tone_pos = 0u;
    s_ring_light = false;
    s_vibra_enabled = true;
    s_stub_vibra_on = false;
    bool ok = tone_next_segment(&st);
    check_segment_invariants(&st, ok, "marker0x0aon");
    assert_true(s_ring_light, "0x0a 0x01 turns ring light on");
    assert_true(s_stub_vibra_on, "0x0a 0x01 turns vibra on when enabled");

    /* off marker 0xfe. */
    static const uint8_t off_stream[] = {0x0au, 0xfeu, 0x4au, 0x10u, 0x0bu};
    init_tone_state(&st, off_stream, sizeof(off_stream), 1u, false, false);
    st.tone_pos = 0u;
    s_ring_light = true;
    s_vibra_enabled = true;
    s_stub_vibra_on = true;
    ok = tone_next_segment(&st);
    check_segment_invariants(&st, ok, "marker0x0aoff");
    assert_true(!s_ring_light, "0x0a 0xfe turns ring light off");
    assert_true(!s_stub_vibra_on, "0x0a 0xfe turns vibra off");
}

/* 0x0a marker with its operand byte missing (opcode at end of stream). Must NOT
 * read OOB; operand stays 0 -> neither light nor vibra change. */
static void test_marker_0x0a_truncated(void) {
    static const uint8_t stream[] = {0x0au};
    audio_state_t st;
    init_tone_state(&st, stream, sizeof(stream), 1u, false, false);
    st.tone_pos = 0u;
    s_ring_light = false;
    s_vibra_enabled = true;
    s_stub_vibra_on = false;
    bool ok = tone_next_segment(&st);
    check_segment_invariants(&st, ok, "marker0x0atrunc");
    assert_true(!ok, "0x0a-only ends the stream (no following note)");
    assert_true(!s_ring_light, "0x0a-only (no operand) leaves light unchanged");
    assert_true(st.tone_pos == 1u, "0x0a-only consumed just the opcode, no OOB");
}

/* Vibra gate: when s_vibra_enabled is false, a 0x0a 0x01 marker must light the
 * ring (not vibra-gated) but must NOT energize the motor. */
static void test_marker_0x0a_vibra_gated(void) {
    static const uint8_t stream[] = {0x0au, 0x01u, 0x4au, 0x10u, 0x0bu};
    audio_state_t st;
    init_tone_state(&st, stream, sizeof(stream), 1u, false, false);
    st.tone_pos = 0u;
    s_ring_light = false;
    s_vibra_enabled = false;
    s_stub_vibra_on = false;
    (void)tone_next_segment(&st);
    assert_true(s_ring_light, "0x0a 0x01 lights ring even with vibra disabled");
    assert_true(!s_stub_vibra_on, "0x0a 0x01 does NOT drive motor when vibra disabled");
}

/* =========================================================================
 * 0x05/0x06 repeat block. "05 02 <note> 06" repeats the note span twice.
 * We point past any header and count produced note segments.
 * ========================================================================= */
static void test_repeat_block_0x05_0x06(void) {
    /* 05 03 (note 4a/10) 06 0b : the single note repeats 3 times. */
    static const uint8_t stream[] = {0x05u, 0x03u, 0x4au, 0x10u, 0x06u, 0x0bu};
    audio_state_t st;
    init_tone_state(&st, stream, sizeof(stream), 1u, false, false);
    st.tone_pos = 0u;
    uint32_t notes = drain_decoder(&st, "repeat0506");
    assert_true(notes == 3u, "05 03 ... 06 repeats the note 3 times");
}

/* Repeat count 0 is treated as 1 (slot->remaining = repeat==0?1:repeat). */
static void test_repeat_count_zero(void) {
    static const uint8_t stream[] = {0x05u, 0x00u, 0x4au, 0x10u, 0x06u, 0x0bu};
    audio_state_t st;
    init_tone_state(&st, stream, sizeof(stream), 1u, false, false);
    st.tone_pos = 0u;
    uint32_t notes = drain_decoder(&st, "repeat0");
    assert_true(notes == 1u, "repeat count 0 plays the body once");
}

/* Repeat nesting beyond AUDIO_TONE_REPEAT_DEPTH (4) must not overflow the
 * tone_repeats[] array: a 5th-deep 0x05 is ignored (depth guard), no OOB. */
static void test_repeat_nesting_overflow(void) {
    /* Five back-to-back 0x05 openers (count 2 each) then a note and closers.
     * The decoder records at most 4; the 5th must be silently dropped. ASan
     * verifies tone_repeats[] is never indexed out of range. */
    static const uint8_t stream[] = {
        0x05u, 0x02u, 0x05u, 0x02u, 0x05u, 0x02u, 0x05u, 0x02u, 0x05u, 0x02u,
        0x4au, 0x10u,
        0x06u, 0x06u, 0x06u, 0x06u, 0x06u, 0x06u,
        0x0bu,
    };
    audio_state_t st;
    init_tone_state(&st, stream, sizeof(stream), 1u, false, false);
    st.tone_pos = 0u;
    /* Bounded drain: just require termination + no sanitizer trip. */
    (void)drain_decoder(&st, "repeatNest");
    assert_true(st.tone_repeat_depth <= AUDIO_TONE_REPEAT_DEPTH, "repeat depth never exceeds cap");
}

/* 0x05 at end-of-stream (no repeat-count operand) must return false, not read
 * past the buffer. */
static void test_repeat_0x05_truncated(void) {
    static const uint8_t stream[] = {0x05u};
    audio_state_t st;
    init_tone_state(&st, stream, sizeof(stream), 1u, false, false);
    st.tone_pos = 0u;
    bool ok = tone_next_segment(&st);
    assert_true(!ok, "0x05 with no count operand returns false");
    check_segment_invariants(&st, ok, "repeat05trunc");
}

/* =========================================================================
 * 0x07 0x0b repeat-whole-stream marker.
 *   - finite restart_count (e.g. 2): the stream replays once, then ends.
 *   - REPEAT_FOREVER (255): must keep restarting (drain hits the guard-bounded
 *     terminate -> we instead bound it ourselves below).
 * ========================================================================= */
static void test_restart_finite_0x07_0x0b(void) {
    /* "00 02 fc 09" header (offset 4 per tone_start_offset) then a note then the
     * 07 0b restart. With restart_count=2 the body should play twice total. */
    static const uint8_t stream[] = {0x00u, 0x02u, 0xfcu, 0x09u, 0x4au, 0x10u, 0x07u, 0x0bu};
    audio_state_t st;
    init_tone_state(&st, stream, sizeof(stream), 2u, false, false);
    /* tone_start_offset for {00,02,fc,09}: data[0]==0,data[1]==0x02,data[2]>=0xfc
     * and data[3]==0x09 -> returns 4. */
    assert_true(st.tone_start == 4u, "header offset = 4 for 00 02 fc 09");
    uint32_t notes = drain_decoder(&st, "restart2");
    assert_true(notes == 2u, "07 0b with restart_count=2 plays the body twice");
}

/* REPEAT_FOREVER must keep restarting; the per-call guard (<128) prevents an
 * infinite loop inside one tone_next_segment call. We drive a bounded number of
 * iterations and confirm it never returns false and never reads OOB. */
static void test_restart_forever(void) {
    static const uint8_t stream[] = {0x00u, 0x02u, 0xfcu, 0x09u, 0x4au, 0x10u, 0x07u, 0x0bu};
    audio_state_t st;
    init_tone_state(&st, stream, sizeof(stream), AUDIO_TONE_REPEAT_FOREVER, false, false);
    for (uint32_t i = 0u; i < 50u; i++) {
        bool ok = tone_next_segment(&st);
        check_segment_invariants(&st, ok, "restartForever");
        assert_true(ok, "REPEAT_FOREVER never ends");
        st.tone_segment_remaining = 0u; /* consume */
    }
}

/* =========================================================================
 * Unknown / control-only opcodes. 0x01,0x02,0x08,0x0e consume one operand byte;
 * 0x0c,0x0d,0x0f,0x10 are no-operand no-ops. None produce a segment on their
 * own. Followed by a note they must still reach it. Truncated (opcode at EOF)
 * they must not read OOB.
 * ========================================================================= */
static void test_one_operand_opcodes(void) {
    const uint8_t ops[] = {0x01u, 0x02u, 0x08u, 0x0eu};
    for (size_t k = 0u; k < sizeof(ops); k++) {
        uint8_t stream[] = {ops[k], 0xaau, 0x4au, 0x10u, 0x0bu};
        audio_state_t st;
        init_tone_state(&st, stream, sizeof(stream), 1u, false, false);
        st.tone_pos = 0u;
        bool ok = tone_next_segment(&st);
        check_segment_invariants(&st, ok, "oneOperand");
        assert_true(ok, "1-operand opcode skips to the following note");
        /* operand 0xaa consumed; note 0x4a decoded. */
        assert_true(st.tone_pos == 4u, "1-operand opcode consumed opcode+operand+note opcode+dur");
        assert_true(st.tone_rest == 0u, "following note is voiced");
    }
}

static void test_one_operand_opcode_truncated(void) {
    const uint8_t ops[] = {0x01u, 0x02u, 0x08u, 0x0eu};
    for (size_t k = 0u; k < sizeof(ops); k++) {
        uint8_t stream[] = {ops[k]}; /* opcode only, operand missing */
        audio_state_t st;
        init_tone_state(&st, stream, sizeof(stream), 1u, false, false);
        st.tone_pos = 0u;
        bool ok = tone_next_segment(&st);
        check_segment_invariants(&st, ok, "oneOperandTrunc");
        assert_true(!ok, "1-operand opcode at EOF ends cleanly");
        assert_true(st.tone_pos == 1u, "1-operand opcode at EOF does not over-read");
    }
}

static void test_noop_opcodes(void) {
    const uint8_t ops[] = {0x0cu, 0x0du, 0x0fu, 0x10u};
    for (size_t k = 0u; k < sizeof(ops); k++) {
        uint8_t stream[] = {ops[k], 0x4au, 0x10u, 0x0bu};
        audio_state_t st;
        init_tone_state(&st, stream, sizeof(stream), 1u, false, false);
        st.tone_pos = 0u;
        bool ok = tone_next_segment(&st);
        check_segment_invariants(&st, ok, "noop");
        assert_true(ok, "no-op opcode falls through to the note");
        assert_true(st.tone_pos == 3u, "no-op consumed just itself before the note");
    }
}

/* =========================================================================
 * Truncated-stream fuzz: take a representative generated ringtone and feed
 * EVERY prefix length to the decoder. The decoder must terminate and never
 * read past tone_len for any truncation (ASan/UBSan enforce the bound).
 * ========================================================================= */
static void test_truncation_sweep_generated_ringtone(void) {
    uint16_t len = 0u;
    const ringtone_t *rt = ringtone_by_index(0u);
    assert_true(rt != 0 && rt->data != 0 && rt->length > 0u, "ringtone 0 available");
    if (rt == 0 || rt->data == 0) {
        return;
    }
    for (uint16_t cut = 0u; cut <= rt->length; cut++) {
        audio_state_t st;
        init_tone_state(&st, rt->data, cut, 1u, true, false);
        s_stub_buzzer_avail = (cut & 1u) != 0u; /* exercise both buzzer/speaker paths */
        (void)len;
        (void)drain_decoder(&st, "truncSweep");
    }
    s_stub_buzzer_avail = false;
}

/* Same sweep across ALL ringtones and all system tones -- broad OOB coverage. */
static void test_truncation_sweep_all_resources(void) {
    uint8_t rcount = ringtone_count();
    for (uint8_t r = 0u; r < rcount; r++) {
        const ringtone_t *rt = ringtone_at(r);
        if (rt == 0 || rt->data == 0) {
            continue;
        }
        for (uint16_t cut = 0u; cut <= rt->length; cut++) {
            audio_state_t st;
            init_tone_state(&st, rt->data, cut, 2u, true, true);
            (void)drain_decoder(&st, "truncAllRt");
        }
    }
    for (uint8_t idx = 0u; idx < 32u; idx++) {
        uint16_t slen = 0u;
        const uint8_t *sd = system_tone_data(idx, &slen);
        if (sd == 0 || slen == 0u) {
            continue;
        }
        for (uint16_t cut = 0u; cut <= slen; cut++) {
            audio_state_t st;
            init_tone_state(&st, sd, cut, 1u, false, false);
            (void)drain_decoder(&st, "truncAllSys");
        }
    }
}

/* =========================================================================
 * Garbage / random bytecode fuzz. A deterministic LCG generates streams of
 * varied lengths and content; the decoder must always terminate (within the
 * 100000-segment cap) and never read OOB. Operand bytes that fall past the end
 * are the exact OOB-trigger surface ASan watches.
 * ========================================================================= */
static void test_garbage_fuzz(void) {
    uint32_t lcg = 0x1234567u;
    for (uint32_t iter = 0u; iter < 4000u; iter++) {
        uint8_t buf[64];
        lcg = lcg * 1103515245u + 12345u;
        uint16_t n = (uint16_t)(lcg % (sizeof(buf) + 1u)); /* 0..64, incl. empty */
        for (uint16_t i = 0u; i < n; i++) {
            lcg = lcg * 1103515245u + 12345u;
            buf[i] = (uint8_t)(lcg >> 16);
        }
        audio_state_t st;
        /* Vary repeats including REPEAT_FOREVER to stress the restart path. */
        uint8_t repeats = (uint8_t)(iter % 7u == 0u ? AUDIO_TONE_REPEAT_FOREVER : (iter % 5u) + 1u);
        bool square = (iter & 1u) != 0u;
        bool dyn = (iter & 2u) != 0u;
        init_tone_state(&st, buf, n, repeats, square, dyn);
        s_stub_buzzer_avail = (iter & 4u) != 0u;
        s_vibra_enabled = (iter & 8u) != 0u;
        s_audio.dynamics = dyn;
        s_env_ceiling = AUDIO_LEVEL_MAX;
        /* Bounded per-stream drain: a REPEAT_FOREVER stream with a real note
         * never terminates, so cap iterations rather than calling drain_decoder. */
        for (uint32_t step = 0u; step < 5000u; step++) {
            bool ok = tone_next_segment(&st);
            check_segment_invariants(&st, ok, "garbageFuzz");
            if (!ok) {
                break;
            }
            st.tone_segment_remaining = 0u;
        }
    }
    s_stub_buzzer_avail = false;
}

/* =========================================================================
 * Full integration through audio_fill(): exercises the real consumer of
 * tone_next_segment (the DMA fill loop) against a generated ringtone, ensuring
 * the decode + envelope + buzzer-routing path produces samples without OOB.
 * ========================================================================= */
static void test_audio_fill_integration(void) {
    const ringtone_t *rt = ringtone_by_index(0u);
    if (rt == 0 || rt->data == 0) {
        return;
    }
    memset(&s_audio, 0, sizeof(s_audio));
    init_tone_state(&s_audio, rt->data, rt->length, 1u, false, false);
    s_audio.amplitude = audio_level_amplitude(AUDIO_LEVEL_MAX);
    int16_t out[64 * 2];
    for (int block = 0; block < 4000 && s_audio.kind == AUDIO_KIND_TONE_STREAM; block++) {
        audio_fill(&s_audio, out, 64u);
    }
    /* A finite (non-looping) ringtone must eventually fall to SILENCE. */
    assert_true(s_audio.kind == AUDIO_KIND_SILENCE, "finite ringtone fill reaches silence");
}

static void test_headset_local_gain_does_not_attenuate_voice(void) {
    int16_t handset[128u * 2u] = {0};
    int16_t headset[128u * 2u] = {0};
    s_stub_bridge_active = false;
    s_stub_downlink_sample = 0;
    s_earpiece_gain_q8 = 256;
    s_click_gain_q8 = AUDIO_KEYPAD_CLICK_GAIN_Q8_DEFAULT;

    s_stub_bridge_route = (uint8_t)NAU_ROUTE_HANDSET;
    start_click(4u);
    audio_fill(&s_audio, handset, 128u);
    s_stub_bridge_route = (uint8_t)NAU_ROUTE_HEADSET;
    start_click(4u);
    audio_fill(&s_audio, headset, 128u);

    bool saw_local_sample = false;
    for (size_t i = 0u; i < sizeof(handset) / sizeof(handset[0]); i++) {
        int32_t expected = headset_local_sample(handset[i]);
        assert_true(headset[i] == expected,
                    "headset route applies exactly -30 dB to local synthesized PCM");
        saw_local_sample |= handset[i] != 0;
    }
    assert_true(saw_local_sample && AUDIO_HEADSET_LOCAL_GAIN_Q15 == 1036u,
                "headset local-gain proof exercises a real waveform and pins its coefficient");

    memset(&s_audio, 0, sizeof(s_audio));
    s_audio.kind = AUDIO_KIND_SILENCE;
    s_stub_bridge_active = true;
    s_stub_downlink_sample = 12000;
    int16_t voice[8u * 2u] = {0};
    audio_fill(&s_audio, voice, 8u);
    for (size_t i = 0u; i < sizeof(voice) / sizeof(voice[0]); i++) {
        assert_true(voice[i] == 12000,
                    "headset local-gain calibration leaves Telit downlink at unity");
    }

    s_stub_downlink_sample = 0;
    s_stub_bridge_active = false;
    s_stub_bridge_route = (uint8_t)NAU_ROUTE_HANDSET;
}

/* The measured production calibration and every volatile override must remain
 * exact and bounded. Release=93 ms is still the legacy full-length ramp and
 * therefore the upper comparison limit, not the production default. */
static void test_keypad_audio_calibration_controls(void) {
    s_stub_bridge_route = (uint8_t)NAU_ROUTE_HANDSET;
    s_click_gain_q8 = AUDIO_KEYPAD_CLICK_GAIN_Q8_DEFAULT;
    s_click_h2_q12 = AUDIO_KEYPAD_CLICK_H2_Q12_DEFAULT;
    s_click_h2_phase = AUDIO_KEYPAD_CLICK_H2_PHASE_Q32_DEFAULT;
    s_click_h3_q12 = AUDIO_KEYPAD_CLICK_H3_Q12_DEFAULT;
    s_click_h3_phase = AUDIO_KEYPAD_CLICK_H3_PHASE_Q32_DEFAULT;
    s_click_h4_q12 = AUDIO_KEYPAD_CLICK_H4_Q12_DEFAULT;
    s_click_h4_phase = AUDIO_KEYPAD_CLICK_H4_PHASE_Q32_DEFAULT;
    s_click_h5_q12 = AUDIO_KEYPAD_CLICK_H5_Q12_DEFAULT;
    s_click_h5_phase = AUDIO_KEYPAD_CLICK_H5_PHASE_Q32_DEFAULT;
    s_click_attack_samples = AUDIO_CLICK_ATTACK_SAMPLES_DEFAULT;
    s_click_release_samples = AUDIO_CLICK_RELEASE_SAMPLES_DEFAULT;
    s_dtmf_gain_q8 = AUDIO_KEYPAD_DTMF_GAIN_Q8_DEFAULT;
    s_dtmf_low_weight = AUDIO_KEYPAD_DTMF_LOW_WEIGHT_DEFAULT;
    s_dtmf_high_weight = AUDIO_KEYPAD_DTMF_HIGH_WEIGHT_DEFAULT;
    s_dtmf_headset_low_weight = AUDIO_KEYPAD_DTMF_HEADSET_LOW_WEIGHT_DEFAULT;
    s_dtmf_headset_high_weight = AUDIO_KEYPAD_DTMF_HEADSET_HIGH_WEIGHT_DEFAULT;

    uint32_t body_sample = AUDIO_CLICK_ATTACK_SAMPLES_DEFAULT + 100u;
    assert_true(s_click_gain_q8 == 238u &&
                    s_click_h2_q12 == 0 && s_click_h2_phase == 0u &&
                    s_click_h3_q12 == 41 &&
                    s_click_h3_phase == 0x416c16c1u &&
                    s_click_h4_q12 == 0 && s_click_h4_phase == 0u &&
                    s_click_h5_q12 == 0 && s_click_h5_phase == 0u &&
                    s_dtmf_gain_q8 == 307u &&
                    s_dtmf_low_weight == 74u && s_dtmf_high_weight == 100u &&
                    s_dtmf_headset_low_weight == 54u &&
                    s_dtmf_headset_high_weight == 100u,
                "production keypad calibration constants remain exact");
    assert_true(keypad_audio_amplitude(1u) == 3259 &&
                    keypad_audio_amplitude(2u) == 7910 &&
                    keypad_audio_amplitude(4u) == 22400,
                "keypad-only level ladder remains exact");
    assert_true((keypad_audio_amplitude(1u) * 10000) /
                        keypad_audio_amplitude(4u) == 1454 &&
                    (keypad_audio_amplitude(2u) * 10000) /
                        keypad_audio_amplitude(4u) == 3531,
                "keypad Level 1/2 ratios match the stock recordings");
    assert_true(click_envelope_q8(body_sample) == 256,
                "production click has a flat sustained body");
    assert_true(click_envelope_q8(AUDIO_CLICK_ATTACK_SAMPLES_DEFAULT / 2u) == 128,
                "production click reaches half scale halfway through its attack");
    assert_true(click_envelope_q8(AUDIO_CLICK_SAMPLES - 1u) > 0 &&
                    click_envelope_q8(AUDIO_CLICK_SAMPLES - 1u) < 16,
                "production click reaches near-zero at the final sample");

    audio_service_command(CORE1_CMD_AUDIO_SET_CLICK_ATTACK_MS, 4u);
    assert_true(s_click_attack_samples == (AUDIO_SAMPLE_RATE * 4u) / 1000u,
                "click attack command converts milliseconds to samples");
    assert_true(click_envelope_q8((AUDIO_SAMPLE_RATE * 2u) / 1000u) == 128,
                "four-millisecond click attack is half-scale at two milliseconds");

    audio_service_command(CORE1_CMD_AUDIO_SET_CLICK_RELEASE_MS, 6u);
    uint32_t short_release = (AUDIO_SAMPLE_RATE * 6u) / 1000u;
    assert_true(s_click_release_samples == short_release,
                "click release command converts milliseconds to samples");
    assert_true(click_envelope_q8(s_click_attack_samples + 100u) == 256,
                "short click release leaves a flat sustained body");
    assert_true(click_envelope_q8(AUDIO_CLICK_SAMPLES - 1u) > 0 &&
                    click_envelope_q8(AUDIO_CLICK_SAMPLES - 1u) < 16,
                "short click release reaches near-zero at the final sample");
    audio_service_command(CORE1_CMD_AUDIO_SET_CLICK_RELEASE_MS, UINT16_MAX);
    assert_true(s_click_release_samples ==
                    (AUDIO_SAMPLE_RATE * AUDIO_CLICK_RELEASE_MS_MAX) / 1000u,
                "click release is capped at the legacy full ramp");

    uint32_t phase = 0x15555555u; /* about 30 degrees: H3 is near its peak. */
    audio_service_command(CORE1_CMD_AUDIO_SET_CLICK_H2, 0u);
    audio_service_command(CORE1_CMD_AUDIO_SET_CLICK_H3, 0u);
    audio_service_command(CORE1_CMD_AUDIO_SET_CLICK_H4, 0u);
    audio_service_command(CORE1_CMD_AUDIO_SET_CLICK_H5, 0u);
    int32_t pure = click_wave_q15(phase);
    assert_true(pure == sine_q15(phase),
                "zero H3 calibration is a bit-faithful pure sine");
    audio_service_command(CORE1_CMD_AUDIO_SET_CLICK_H3, 320u);
    assert_true(s_click_h3_q12 == 320 && click_wave_q15(phase) != pure,
                "positive H3 calibration changes click timbre");
    int32_t h3_phase_zero = click_wave_q15(phase);
    audio_service_command(CORE1_CMD_AUDIO_SET_CLICK_H3_PHASE, 90u);
    assert_true(s_click_h3_phase == 0x40000000u &&
                    click_wave_q15(phase) != h3_phase_zero,
                "H3 phase calibration changes shape with an exact quarter turn");
    audio_service_command(CORE1_CMD_AUDIO_SET_CLICK_H3_PHASE,
                          (uint16_t)(int16_t)-500);
    assert_true(s_click_h3_phase == 0x80000000u,
                "H3 phase calibration is signed and bounded to a half turn");
    audio_service_command(CORE1_CMD_AUDIO_SET_CLICK_H3, (uint16_t)(int16_t)-5000);
    assert_true(s_click_h3_q12 == -AUDIO_CLICK_HARMONIC_Q12_MAX,
                "negative H3 calibration is signed and bounded");

    audio_service_command(CORE1_CMD_AUDIO_SET_CLICK_H2, 256u);
    audio_service_command(CORE1_CMD_AUDIO_SET_CLICK_H2_PHASE, 45u);
    assert_true(s_click_h2_q12 == 256 && s_click_h2_phase == 0x20000000u,
                "H2 calibration installs magnitude and phase");
    audio_service_command(CORE1_CMD_AUDIO_SET_CLICK_H4, 384u);
    audio_service_command(CORE1_CMD_AUDIO_SET_CLICK_H4_PHASE,
                          (uint16_t)(int16_t)-90);
    assert_true(s_click_h4_q12 == 384 && s_click_h4_phase == 0xc0000000u,
                "H4 calibration installs signed phase");
    audio_service_command(CORE1_CMD_AUDIO_SET_CLICK_H5, UINT16_MAX);
    audio_service_command(CORE1_CMD_AUDIO_SET_CLICK_H5_PHASE, 500u);
    assert_true(s_click_h5_q12 == -1 && s_click_h5_phase == 0x80000000u,
                "H5 calibration preserves signed amplitude and bounds phase");

    audio_service_command(CORE1_CMD_AUDIO_SET_CLICK_H2, UINT16_MAX / 2u);
    audio_service_command(CORE1_CMD_AUDIO_SET_CLICK_H3, UINT16_MAX / 2u);
    audio_service_command(CORE1_CMD_AUDIO_SET_CLICK_H4, UINT16_MAX / 2u);
    audio_service_command(CORE1_CMD_AUDIO_SET_CLICK_H5, UINT16_MAX / 2u);
    assert_true(s_click_h2_q12 == AUDIO_CLICK_HARMONIC_Q12_MAX &&
                    s_click_h3_q12 == AUDIO_CLICK_HARMONIC_Q12_MAX &&
                    s_click_h4_q12 == AUDIO_CLICK_HARMONIC_Q12_MAX &&
                    s_click_h5_q12 == AUDIO_CLICK_HARMONIC_Q12_MAX,
                "every click harmonic is bounded at fifty percent");
    for (uint32_t sample_phase = 0u;; sample_phase += 0x01000000u) {
        int32_t shaped = click_wave_q15(sample_phase);
        assert_true(shaped >= -32767 && shaped <= 32767,
                    "multi-harmonic click remains inside Q15 bounds");
        if (sample_phase == 0xff000000u) {
            break;
        }
    }

    audio_service_command(CORE1_CMD_AUDIO_SET_CLICK_GAIN, UINT16_MAX);
    audio_service_command(CORE1_CMD_AUDIO_SET_DTMF_GAIN, UINT16_MAX);
    assert_true(s_click_gain_q8 == AUDIO_KEYPAD_GAIN_Q8_MAX &&
                    s_dtmf_gain_q8 == AUDIO_KEYPAD_GAIN_Q8_MAX,
                "keypad calibration gains are bounded at 2x");

    s_click_gain_q8 = AUDIO_KEYPAD_CLICK_GAIN_Q8_DEFAULT;
    s_click_h2_q12 = AUDIO_KEYPAD_CLICK_H2_Q12_DEFAULT;
    s_click_h2_phase = AUDIO_KEYPAD_CLICK_H2_PHASE_Q32_DEFAULT;
    s_click_h3_q12 = AUDIO_KEYPAD_CLICK_H3_Q12_DEFAULT;
    s_click_h3_phase = AUDIO_KEYPAD_CLICK_H3_PHASE_Q32_DEFAULT;
    s_click_h4_q12 = AUDIO_KEYPAD_CLICK_H4_Q12_DEFAULT;
    s_click_h4_phase = AUDIO_KEYPAD_CLICK_H4_PHASE_Q32_DEFAULT;
    s_click_h5_q12 = AUDIO_KEYPAD_CLICK_H5_Q12_DEFAULT;
    s_click_h5_phase = AUDIO_KEYPAD_CLICK_H5_PHASE_Q32_DEFAULT;
    s_click_attack_samples = AUDIO_CLICK_ATTACK_SAMPLES_DEFAULT;
    s_click_release_samples = AUDIO_CLICK_RELEASE_SAMPLES_DEFAULT;
    s_dtmf_gain_q8 = AUDIO_KEYPAD_DTMF_GAIN_Q8_DEFAULT;
    s_dtmf_low_weight = AUDIO_KEYPAD_DTMF_LOW_WEIGHT_DEFAULT;
    s_dtmf_high_weight = AUDIO_KEYPAD_DTMF_HIGH_WEIGHT_DEFAULT;
    s_dtmf_headset_low_weight = AUDIO_KEYPAD_DTMF_HEADSET_LOW_WEIGHT_DEFAULT;
    s_dtmf_headset_high_weight = AUDIO_KEYPAD_DTMF_HEADSET_HIGH_WEIGHT_DEFAULT;

    audio_service_command(CORE1_CMD_AUDIO_SET_DTMF_WEIGHTS,
                          (uint16_t)((100u << 8) | 74u));
    assert_true(s_dtmf_low_weight == 74u && s_dtmf_high_weight == 100u,
                "DTMF calibration installs independent low/high weights");
    audio_service_command(CORE1_CMD_AUDIO_SET_DTMF_WEIGHTS, 0u);
    assert_true(s_dtmf_low_weight == 74u && s_dtmf_high_weight == 100u,
                "all-zero DTMF weights cannot create a divide-by-zero state");
    audio_service_command(CORE1_CMD_AUDIO_SET_DTMF_HEADSET_WEIGHTS,
                          (uint16_t)((100u << 8) | 54u));
    assert_true(s_dtmf_headset_low_weight == 54u &&
                    s_dtmf_headset_high_weight == 100u &&
                    s_dtmf_low_weight == 74u && s_dtmf_high_weight == 100u,
                "headset DTMF calibration remains independent of handset weights");
    audio_service_command(CORE1_CMD_AUDIO_SET_DTMF_HEADSET_WEIGHTS, 0u);
    assert_true(s_dtmf_headset_low_weight == 54u &&
                    s_dtmf_headset_high_weight == 100u,
                "all-zero headset DTMF weights cannot create a divide-by-zero state");

    /* The synth must select the calibrated pair from the live route. Use
     * orthogonal one-tone mixes so a stale/common pair cannot compare equal. */
    s_dtmf_gain_q8 = 256u;
    s_dtmf_low_weight = 1u;
    s_dtmf_high_weight = 0u;
    s_dtmf_headset_low_weight = 0u;
    s_dtmf_headset_high_weight = 1u;
    int16_t handset_mix[32 * 2] = {0};
    int16_t headset_mix[32 * 2] = {0};
    s_stub_bridge_route = (uint8_t)NAU_ROUTE_HANDSET;
    start_dtmf('2', 4u);
    audio_fill(&s_audio, handset_mix, 32u);
    s_stub_bridge_route = (uint8_t)NAU_ROUTE_HEADSET;
    start_dtmf('2', 4u);
    audio_fill(&s_audio, headset_mix, 32u);
    assert_true(memcmp(handset_mix, headset_mix, sizeof(handset_mix)) != 0,
                "DTMF synth selects distinct handset and headset mixes");

    s_stub_bridge_route = (uint8_t)NAU_ROUTE_HANDSET;
    s_dtmf_gain_q8 = AUDIO_KEYPAD_DTMF_GAIN_Q8_DEFAULT;
    s_dtmf_low_weight = AUDIO_KEYPAD_DTMF_LOW_WEIGHT_DEFAULT;
    s_dtmf_high_weight = AUDIO_KEYPAD_DTMF_HIGH_WEIGHT_DEFAULT;
    s_dtmf_headset_low_weight = AUDIO_KEYPAD_DTMF_HEADSET_LOW_WEIGHT_DEFAULT;
    s_dtmf_headset_high_weight = AUDIO_KEYPAD_DTMF_HEADSET_HIGH_WEIGHT_DEFAULT;

    start_click(4u);
    int16_t click_out[128 * 2] = {0};
    audio_fill(&s_audio, click_out, 128u);
    int32_t click_peak = 0;
    for (size_t i = 0u; i < sizeof(click_out) / sizeof(click_out[0]); i++) {
        int32_t magnitude = click_out[i] < 0 ? -(int32_t)click_out[i] : click_out[i];
        if (magnitude > click_peak) {
            click_peak = magnitude;
        }
    }
    assert_true(click_peak > 1000 && click_peak < 32767,
                "production Level-3 click is audible without PCM clipping");

    start_dtmf('2', 4u);
    int16_t live_out[128 * 2] = {0};
    audio_fill(&s_audio, live_out, 128u);
    bool live_nonzero = false;
    int32_t live_peak = 0;
    for (size_t i = 0u; i < sizeof(live_out) / sizeof(live_out[0]); i++) {
        live_nonzero |= live_out[i] != 0;
        int32_t magnitude = live_out[i] < 0 ? -(int32_t)live_out[i] : live_out[i];
        if (magnitude > live_peak) {
            live_peak = magnitude;
        }
    }
    assert_true(s_audio.kind == AUDIO_KIND_DTMF &&
                    s_audio.sample_index == 128u && live_nonzero &&
                    live_peak < 32767,
                "production DTMF renders without clipping before its ceiling");
    assert_true(AUDIO_KEYPAD_DTMF_MAX_HOLD_MS == 3840u &&
                    AUDIO_DTMF_MAX_SAMPLES == 61440u,
                "held DTMF ceiling matches the stock long-hold calibration");
    s_audio.sample_index = AUDIO_DTMF_MAX_SAMPLES - 1u;
    int16_t last_live_out[2] = {0, 0};
    audio_fill(&s_audio, last_live_out, 1u);
    assert_true(s_audio.kind == AUDIO_KIND_DTMF &&
                    s_audio.sample_index == AUDIO_DTMF_MAX_SAMPLES,
                "held DTMF renders the final sample before its ceiling");
    int16_t cutoff_out[2] = {1, 1};
    audio_fill(&s_audio, cutoff_out, 1u);
    assert_true(s_audio.kind == AUDIO_KIND_SILENCE &&
                    s_audio.active_key == 0 &&
                    cutoff_out[0] == 0 && cutoff_out[1] == 0,
                "held DTMF stops cleanly at the stock 3.84-second ceiling");

    s_click_gain_q8 = AUDIO_KEYPAD_CLICK_GAIN_Q8_DEFAULT;
    s_click_h2_q12 = AUDIO_KEYPAD_CLICK_H2_Q12_DEFAULT;
    s_click_h2_phase = AUDIO_KEYPAD_CLICK_H2_PHASE_Q32_DEFAULT;
    s_click_h3_q12 = AUDIO_KEYPAD_CLICK_H3_Q12_DEFAULT;
    s_click_h3_phase = AUDIO_KEYPAD_CLICK_H3_PHASE_Q32_DEFAULT;
    s_click_h4_q12 = AUDIO_KEYPAD_CLICK_H4_Q12_DEFAULT;
    s_click_h4_phase = AUDIO_KEYPAD_CLICK_H4_PHASE_Q32_DEFAULT;
    s_click_h5_q12 = AUDIO_KEYPAD_CLICK_H5_Q12_DEFAULT;
    s_click_h5_phase = AUDIO_KEYPAD_CLICK_H5_PHASE_Q32_DEFAULT;
    s_click_attack_samples = AUDIO_CLICK_ATTACK_SAMPLES_DEFAULT;
    s_click_release_samples = AUDIO_CLICK_RELEASE_SAMPLES_DEFAULT;
    s_dtmf_gain_q8 = AUDIO_KEYPAD_DTMF_GAIN_Q8_DEFAULT;
    s_dtmf_low_weight = AUDIO_KEYPAD_DTMF_LOW_WEIGHT_DEFAULT;
    s_dtmf_high_weight = AUDIO_KEYPAD_DTMF_HIGH_WEIGHT_DEFAULT;
    s_dtmf_headset_low_weight = AUDIO_KEYPAD_DTMF_HEADSET_LOW_WEIGHT_DEFAULT;
    s_dtmf_headset_high_weight = AUDIO_KEYPAD_DTMF_HEADSET_HIGH_WEIGHT_DEFAULT;
}

/* Headset buzzer-duplication (real-3210 bench 2026-07-10): with a headset
 * inserted, buzzer-path sounds play on BOTH the buzzer AND the headset. Oracle:
 * a square stream with the buzzer fitted renders SILENT I2S samples on the
 * handset route, and NON-silent samples on the headset route -- while the
 * buzzer keeps being driven in both cases. */
static void test_square_stream_headset_duplication(void) {
    /* One audible note (0x4a = the known-valid pitch used by the pitch test),
     * long enough to span several fill blocks. */
    static const uint8_t melody[] = {0x4au, 0x30u, 0x0bu};
    bool saved_avail = s_stub_buzzer_avail;
    s_stub_buzzer_avail = true;
    int16_t out[64 * 2];

    /* Handset route: buzzer-only, I2S copy muted. */
    s_stub_bridge_route = (uint8_t)NAU_ROUTE_HANDSET;
    s_stub_buzzer_freq_set = false;
    memset(&s_audio, 0, sizeof(s_audio));
    init_tone_state(&s_audio, melody, sizeof(melody), 1u, true, false);
    s_audio.amplitude = audio_level_amplitude(AUDIO_LEVEL_MAX);
    bool any_nonzero = false;
    for (int block = 0; block < 4 && s_audio.kind == AUDIO_KIND_TONE_STREAM; block++) {
        memset(out, 0, sizeof(out));
        audio_fill(&s_audio, out, 64u);
        for (unsigned i = 0; i < 64u * 2u; i++) {
            if (out[i] != 0) {
                any_nonzero = true;
            }
        }
    }
    assert_true(!any_nonzero, "handset route: square stream stays off the I2S path");
    assert_true(s_stub_buzzer_freq_set, "handset route: buzzer driven");

    /* Headset route: SAME stream also renders on the I2S path (to the HP pair). */
    s_stub_bridge_route = (uint8_t)NAU_ROUTE_HEADSET;
    s_stub_buzzer_freq_set = false;
    memset(&s_audio, 0, sizeof(s_audio));
    init_tone_state(&s_audio, melody, sizeof(melody), 1u, true, false);
    s_audio.amplitude = audio_level_amplitude(AUDIO_LEVEL_MAX);
    any_nonzero = false;
    for (int block = 0; block < 4 && s_audio.kind == AUDIO_KIND_TONE_STREAM; block++) {
        memset(out, 0, sizeof(out));
        audio_fill(&s_audio, out, 64u);
        for (unsigned i = 0; i < 64u * 2u; i++) {
            if (out[i] != 0) {
                any_nonzero = true;
            }
        }
    }
    assert_true(any_nonzero, "headset route: square stream duplicates onto the I2S path");
    assert_true(s_stub_buzzer_freq_set, "headset route: buzzer still driven too");

    /* In-call (voice bridge active): the ROM keeps mid-call alerts off the ear
     * (0x297a8c not-in-call gate) -- the duplication must stay muted. */
    s_stub_bridge_active = true;
    memset(&s_audio, 0, sizeof(s_audio));
    init_tone_state(&s_audio, melody, sizeof(melody), 1u, true, false);
    s_audio.amplitude = audio_level_amplitude(AUDIO_LEVEL_MAX);
    any_nonzero = false;
    for (int block = 0; block < 4 && s_audio.kind == AUDIO_KIND_TONE_STREAM; block++) {
        memset(out, 0, sizeof(out));
        audio_fill(&s_audio, out, 64u);
        for (unsigned i = 0; i < 64u * 2u; i++) {
            if (out[i] != 0) {
                any_nonzero = true;
            }
        }
    }
    assert_true(!any_nonzero, "headset route in-call: duplication gated off the ear");
    s_stub_bridge_active = false;

    s_stub_bridge_route = (uint8_t)NAU_ROUTE_HANDSET;
    s_stub_buzzer_avail = saved_avail;
}

/* tone_start_offset oracle: verify the documented header-skip cases directly. */
static void test_tone_start_offset_cases(void) {
    const uint8_t a[] = {0x00u, 0x00u, 0x02u, 0xfdu, 0x09u}; /* 5-byte header */
    assert_true(tone_start_offset(a, sizeof(a)) == 5u, "00 00 02 fd .. -> 5");
    const uint8_t b[] = {0x00u, 0x02u, 0xfcu, 0x09u};       /* -> 4 (data[3]==0x09) */
    assert_true(tone_start_offset(b, sizeof(b)) == 4u, "00 02 fc 09 -> 4");
    const uint8_t c[] = {0x00u, 0x02u, 0xfcu, 0x4au};       /* -> 3 (data[3]!=0x09) */
    assert_true(tone_start_offset(c, sizeof(c)) == 3u, "00 02 fc .. -> 3");
    const uint8_t d[] = {0x00u, 0x09u, 0xfeu};              /* -> 3 */
    assert_true(tone_start_offset(d, sizeof(d)) == 3u, "00 09 fe -> 3");
    const uint8_t e[] = {0x00u, 0x01u, 0x4au};              /* data[2]<0xfc -> 2 */
    assert_true(tone_start_offset(e, sizeof(e)) == 2u, "00 01 .. -> 2");
    const uint8_t f[] = {0x00u, 0x40u};                     /* rest header -> 1 */
    assert_true(tone_start_offset(f, sizeof(f)) == 1u, "00 40 -> 1");
    const uint8_t g[] = {0x12u, 0x34u};                     /* no header -> 0 */
    assert_true(tone_start_offset(g, sizeof(g)) == 0u, "unknown header -> 0");
    /* Short buffers must not over-read in tone_start_offset. */
    const uint8_t h[] = {0x00u};
    assert_true(tone_start_offset(h, sizeof(h)) == 0u, "1-byte buffer header -> 0, no OOB");
    assert_true(tone_start_offset(h, 0u) == 0u, "0-length header -> 0, no OOB");
}

/* Empty / null stream guards on the decoder. */
static void test_decoder_empty_and_null(void) {
    audio_state_t st;
    init_tone_state(&st, (const uint8_t *)"", 0u, 1u, false, false);
    assert_true(!tone_next_segment(&st), "zero-length stream yields no segment");
    memset(&st, 0, sizeof(st));
    st.kind = AUDIO_KIND_TONE_STREAM;
    st.tone_data = 0;
    st.tone_len = 10u;
    assert_true(!tone_next_segment(&st), "null tone_data yields no segment");
}

static uint16_t test_audio_arg(uint8_t code, uint8_t level) {
    return (uint16_t)(code | ((uint16_t)level << 8u));
}

static uint16_t test_audio_arg_with_marker_vibra(uint8_t code, uint8_t level,
                                                  bool enabled) {
    uint16_t arg = test_audio_arg(code, level);
    return enabled ? (uint16_t)(arg | 0x8000u) : arg;
}

static void test_buzzer_command_routing_contract(void) {
    const uint16_t loud = test_audio_arg(1u, AUDIO_LEVEL_MAX);
    assert_true(audio_service_command_uses_buzzer(CORE1_CMD_AUDIO_RINGTONE_PREVIEW, loud),
                "incoming ring preview requests +3V8");
    assert_true(audio_service_command_uses_buzzer(CORE1_CMD_AUDIO_RINGTONE_LOOP, loud),
                "incoming ring loop requests +3V8");
    assert_true(audio_service_command_uses_buzzer(CORE1_CMD_AUDIO_RINGTONE_MENU_PREVIEW, loud),
                "ringtone menu preview requests +3V8");
    assert_true(audio_service_command_uses_buzzer(CORE1_CMD_AUDIO_DEBUG_BUZZER_TEST, loud),
                "debug buzzer test requests +3V8");
    assert_true(audio_service_command_uses_buzzer(CORE1_CMD_AUDIO_PACMAN_TONE, loud),
                "game tone requests +3V8");
    assert_true(audio_service_command_uses_buzzer(CORE1_CMD_AUDIO_COMPOSER_PACKED, loud),
                "packed own tone requests +3V8");
    assert_true(audio_service_command_uses_buzzer(CORE1_CMD_AUDIO_COMPOSER_PACKED_LOOP, loud),
                "looping own tone requests +3V8");

    static const uint8_t buzzer_system[] = {3u, 5u, 7u, 8u, 9u, 12u, 16u,
                                             17u, 18u, 19u, 20u, 29u, 30u, 32u};
    for (size_t i = 0u; i < sizeof(buzzer_system); i++) {
        assert_true(audio_service_command_uses_buzzer(
                        CORE1_CMD_AUDIO_SYSTEM_TONE,
                        test_audio_arg(buzzer_system[i], AUDIO_LEVEL_MAX)),
                    "buzzer-class system tone requests +3V8");
    }

    static const uint8_t earpiece_system[] = {0u, 10u, 11u, 14u, 21u, 28u, 31u};
    for (size_t i = 0u; i < sizeof(earpiece_system); i++) {
        assert_true(!audio_service_command_uses_buzzer(
                        CORE1_CMD_AUDIO_SYSTEM_TONE,
                        test_audio_arg(earpiece_system[i], AUDIO_LEVEL_MAX)),
                    "earpiece-class system tone leaves +3V8 alone");
    }
    assert_true(!audio_service_command_uses_buzzer(
                    CORE1_CMD_AUDIO_SYSTEM_TONE_QUIET,
                    test_audio_arg(14u, 1u)),
                "quiet in-call notification remains earpiece-only");
    assert_true(!audio_service_command_uses_buzzer(
                    CORE1_CMD_AUDIO_SYSTEM_TONE_QUIET_LOOP,
                    test_audio_arg(28u, AUDIO_LEVEL_MAX)),
                "quiet outgoing ringback remains earpiece-only");
    assert_true(!audio_service_command_uses_buzzer(CORE1_CMD_AUDIO_CLICK, loud),
                "key click remains earpiece-only");
    assert_true(!audio_service_command_uses_buzzer(CORE1_CMD_AUDIO_DTMF, loud),
                "DTMF remains earpiece-only");
    assert_true(!audio_service_command_uses_buzzer(CORE1_CMD_AUDIO_COMPOSER_NOTE, loud),
                "composer editor note remains earpiece-only");
    assert_true(!audio_service_command_uses_buzzer(
                    CORE1_CMD_AUDIO_RINGTONE_LOOP,
                    test_audio_arg(1u, AUDIO_LEVEL_SILENT)),
                "silent ringtone does not energize +3V8");
}

static void test_call_tones_have_isolated_quiet_gain(void) {
    const uint16_t tone14_level1 = test_audio_arg(14u, 1u);

    audio_service_command(CORE1_CMD_AUDIO_SYSTEM_TONE, tone14_level1);
    assert_true(s_audio.kind == AUDIO_KIND_TONE_STREAM,
                "ordinary tone 14 starts a tone stream");
    assert_true(s_audio.amplitude == audio_level_amplitude(1u),
                "ordinary level-1 system tone retains the global amplitude");

    audio_service_command(CORE1_CMD_AUDIO_SYSTEM_TONE_QUIET, tone14_level1);
    assert_true(s_audio.kind == AUDIO_KIND_TONE_STREAM,
                "quiet in-call tone 14 starts a tone stream");
    assert_true(s_audio.amplitude == AUDIO_QUIET_SYSTEM_TONE_AMPLITUDE,
                "in-call notification uses its dedicated sub-level-1 amplitude");
    assert_true(s_audio.amplitude < audio_level_amplitude(1u),
                "quiet notification does not weaken global level 1");

    audio_service_command(CORE1_CMD_AUDIO_SYSTEM_TONE_QUIET_LOOP,
                          test_audio_arg(28u, AUDIO_LEVEL_MAX));
    assert_true(s_audio.kind == AUDIO_KIND_TONE_STREAM,
                "quiet outgoing ringback starts a tone stream");
    assert_true(s_audio.amplitude == AUDIO_QUIET_SYSTEM_TONE_AMPLITUDE,
                "outgoing ringback shares the bench-confirmed quiet gain");
    assert_true(s_audio.tone_restart_count == AUDIO_TONE_REPEAT_FOREVER,
                "quiet outgoing ringback retains looping semantics");
    audio_service_command(CORE1_CMD_AUDIO_STOP, 0u);
}

static void test_debug_output_ownership(void) {
    const uint16_t loud = test_audio_arg(0u, AUDIO_LEVEL_MAX);
    const uint16_t pulse = (uint16_t)(48u | ((uint16_t)5u << 8u));

    s_stub_buzzer_duty_percent = UINT8_MAX;
    audio_service_command(CORE1_CMD_AUDIO_SET_BUZZER_DUTY, 7u);
    assert_true(s_stub_buzzer_duty_percent == 7u,
                "debug buzzer duty accepts an exact bench value");
    audio_service_command(CORE1_CMD_AUDIO_SET_BUZZER_DUTY, 99u);
    assert_true(s_stub_buzzer_duty_percent == 50u,
                "debug buzzer duty cannot exceed the bench safety bound");
    audio_service_command(CORE1_CMD_AUDIO_SET_BUZZER_DUTY, UINT16_MAX);
    assert_true(s_stub_buzzer_duty_percent == UINT8_MAX,
                "debug buzzer duty restores the production curve");

    audio_service_command(CORE1_CMD_AUDIO_STOP, 0u);
    audio_service_command(CORE1_CMD_AUDIO_DEBUG_BUZZER_TEST, loud);
    assert_true(s_debug_buzzer_owned && s_audio.kind != AUDIO_KIND_SILENCE,
                "debug buzzer owns the stream it starts");
    audio_service_command(CORE1_CMD_AUDIO_STOP,
                          test_audio_arg(1u, AUDIO_LEVEL_SILENT));
    assert_true(s_debug_buzzer_owned && s_audio.kind != AUDIO_KIND_SILENCE,
                "unrelated key-up stop does not orphan the debug stream");
    audio_service_command(CORE1_CMD_AUDIO_RINGTONE_LOOP, loud);
    assert_true(!s_debug_buzzer_owned && s_audio.kind != AUDIO_KIND_SILENCE,
                "production ring supersedes debug buzzer ownership");
    audio_service_command(CORE1_CMD_AUDIO_DEBUG_STOP, 0u);
    assert_true(s_audio.kind != AUDIO_KIND_SILENCE,
                "late debug cleanup preserves the production ring");

    audio_service_command(CORE1_CMD_AUDIO_STOP, 0u);
    audio_service_command(CORE1_CMD_AUDIO_DEBUG_VIBRA_TEST, pulse);
    assert_true(s_debug_vibra_owned && s_vibra_pulse_loop,
                "debug vibra owns the pulse loop it starts");
    audio_service_command(CORE1_CMD_AUDIO_VIBRA_PULSE_LOOP, pulse);
    assert_true(!s_debug_vibra_owned && s_vibra_pulse_loop,
                "production pulse supersedes debug vibra ownership");
    audio_service_command(CORE1_CMD_AUDIO_DEBUG_STOP, 0u);
    assert_true(s_vibra_pulse_loop,
                "late debug cleanup preserves the production pulse");

    audio_service_command(CORE1_CMD_AUDIO_STOP, 0u);
    audio_service_command(CORE1_CMD_AUDIO_DEBUG_BUZZER_TEST, loud);
    audio_service_command(CORE1_CMD_AUDIO_DEBUG_STOP, 0u);
    assert_true(!s_debug_buzzer_owned && s_audio.kind == AUDIO_KIND_SILENCE,
                "debug cleanup stops an unclaimed buzzer test");
    audio_service_command(CORE1_CMD_AUDIO_DEBUG_VIBRA_TEST, pulse);
    audio_service_command(CORE1_CMD_AUDIO_DEBUG_STOP, 0u);
    assert_true(!s_debug_vibra_owned && !s_vibra_pulse_active &&
                    !s_vibra_pulse_loop,
                "debug cleanup stops an unclaimed vibra test");
}

static void test_marker_vibra_is_owned_by_stream_start(void) {
    const uint16_t marked_ring =
        test_audio_arg_with_marker_vibra(0u, AUDIO_LEVEL_MAX, true);
    const uint16_t marked_sine =
        test_audio_arg_with_marker_vibra(10u, AUDIO_LEVEL_MAX, true);
    const uint16_t plain_message =
        test_audio_arg_with_marker_vibra(29u, AUDIO_LEVEL_MAX, false);
    const uint16_t debug_pulse =
        (uint16_t)(48u | ((uint16_t)AUDIO_VIBRA_STRENGTH_STOCK << 8u));

    audio_service_command(CORE1_CMD_AUDIO_STOP, 0u);
    audio_service_command(CORE1_CMD_AUDIO_DEBUG_VIBRA_TEST, debug_pulse);
    assert_true(s_debug_vibra_owned && s_vibra_pulse_active,
                "precondition: debug test owns a live pulse");
    audio_service_command(CORE1_CMD_AUDIO_RINGTONE_LOOP, marked_ring);
    assert_true(s_audio.kind == AUDIO_KIND_TONE_STREAM && s_vibra_enabled &&
                    !s_debug_vibra_owned && !s_vibra_pulse_active,
                "ring start atomically publishes its marker-vibra gate");
    assert_true(s_stub_vibra_strength == AUDIO_VIBRA_STRENGTH_STOCK,
                "marker-vibra stream selects the stock motor strength");

    /* Model the old stream completing immediately before the next command is
     * dispatched. The next stream must carry and restore its own gate. */
    finish_tone_stream(&s_audio);
    assert_true(!s_vibra_enabled, "old stream completion retires only its gate");
    audio_service_command(CORE1_CMD_AUDIO_RINGTONE_LOOP, marked_ring);
    assert_true(s_audio.kind == AUDIO_KIND_TONE_STREAM && s_vibra_enabled,
                "new ring re-arms after an interleaved prior completion");

    audio_service_command(CORE1_CMD_AUDIO_SYSTEM_TONE, marked_sine);
    assert_true(s_audio.kind == AUDIO_KIND_TONE_STREAM &&
                    !s_audio.sequence_square && s_vibra_enabled,
                "sine stream preserves the marker gate installed at its start");

    audio_service_command(CORE1_CMD_AUDIO_SYSTEM_TONE, plain_message);
    assert_true(s_audio.kind == AUDIO_KIND_TONE_STREAM && !s_vibra_enabled,
                "unmarked message stream cannot inherit a prior ring gate");
}

static void test_composer_preview_stop_is_owner_scoped(void) {
    const uint16_t note = test_audio_arg(0x7eu, 3u);
    const uint16_t ring = test_audio_arg(0u, AUDIO_LEVEL_MAX);
    const uint16_t alarm = test_audio_arg(12u, AUDIO_LEVEL_MAX);

    audio_service_command(CORE1_CMD_AUDIO_STOP, 0u);
    audio_service_command(CORE1_CMD_AUDIO_COMPOSER_NOTE, note);
    assert_true(s_audio.kind == AUDIO_KIND_COMPOSER_NOTE,
                "Composer preview starts a Composer-owned note");
    audio_service_command(CORE1_CMD_AUDIO_COMPOSER_PREVIEW_STOP, 0u);
    assert_true(s_audio.kind == AUDIO_KIND_SILENCE,
                "Composer preview stop retires its own note");

    audio_service_command(CORE1_CMD_AUDIO_COMPOSER_NOTE, note);
    audio_service_command(CORE1_CMD_AUDIO_RINGTONE_LOOP, ring);
    audio_service_command(CORE1_CMD_AUDIO_COMPOSER_PREVIEW_STOP, 0u);
    assert_true(s_audio.kind == AUDIO_KIND_TONE_STREAM,
                "late Composer stop preserves a replacement incoming ringtone");

    audio_service_command(CORE1_CMD_AUDIO_COMPOSER_NOTE, note);
    audio_service_command(CORE1_CMD_AUDIO_SYSTEM_TONE_LOOP, alarm);
    audio_service_command(CORE1_CMD_AUDIO_COMPOSER_PREVIEW_STOP, 0u);
    assert_true(s_audio.kind == AUDIO_KIND_TONE_STREAM,
                "late Composer stop preserves a replacement alarm stream");
}

static void test_composer_navigation_stop_is_owner_scoped(void) {
    const uint16_t note = test_audio_arg(0x7eu, 3u);
    const uint16_t click = test_audio_arg(0u, 2u);
    const uint16_t ring = test_audio_arg(0u, AUDIO_LEVEL_MAX);
    static const composer_note_event_t packed_note = {1u, 1u, 2u, false};
    uint8_t packed[32];
    uint16_t packed_len = 0u;
    bool encoded = composer_codec_encode("Own", &packed_note, 1u, 0x0cu,
                                         packed, sizeof(packed), &packed_len);
    assert_true(encoded, "packed Composer stop fixture encodes");

    audio_service_command(CORE1_CMD_AUDIO_STOP, 0u);
    audio_service_command(CORE1_CMD_AUDIO_COMPOSER_NOTE, note);
    audio_service_command(CORE1_CMD_AUDIO_COMPOSER_STOP, 0u);
    assert_true(s_audio.kind == AUDIO_KIND_SILENCE,
                "Composer navigation stop retires a key preview");

    audio_service_start_composer_packed(packed, packed_len, 3u);
    assert_true(s_audio.kind == AUDIO_KIND_COMPOSER_PACKED,
                "packed Composer fixture starts playback");
    audio_service_command(CORE1_CMD_AUDIO_COMPOSER_STOP, 0u);
    assert_true(s_audio.kind == AUDIO_KIND_SILENCE,
                "Composer navigation stop retires packed playback");

    audio_service_command(CORE1_CMD_AUDIO_COMPOSER_NOTE, note);
    audio_service_command(CORE1_CMD_AUDIO_CLICK, click);
    audio_service_command(CORE1_CMD_AUDIO_COMPOSER_STOP, 0u);
    assert_true(s_audio.kind == AUDIO_KIND_CLICK,
                "Composer navigation stop preserves its queued keypad click");

    audio_service_command(CORE1_CMD_AUDIO_COMPOSER_NOTE, note);
    audio_service_command(CORE1_CMD_AUDIO_RINGTONE_LOOP, ring);
    audio_service_command(CORE1_CMD_AUDIO_COMPOSER_STOP, 0u);
    assert_true(s_audio.kind == AUDIO_KIND_TONE_STREAM,
                "Composer navigation stop preserves a replacement ringtone");
}

static void test_tones_navigation_stop_is_owner_scoped(void) {
    const uint16_t ring = test_audio_arg(0u, AUDIO_LEVEL_MAX);
    const uint16_t message = test_audio_arg(29u, 3u);
    const uint16_t click = test_audio_arg(0u, 2u);

    audio_service_command(CORE1_CMD_AUDIO_STOP, 0u);
    audio_service_command(CORE1_CMD_AUDIO_RINGTONE_MENU_PREVIEW, ring);
    assert_true(s_tones_preview_owned &&
                    s_audio.kind == AUDIO_KIND_TONE_STREAM,
                "Tones ringtone preview owns the stream it starts");
    audio_service_command(CORE1_CMD_AUDIO_TONES_PREVIEW_STOP, 0u);
    assert_true(!s_tones_preview_owned &&
                    s_audio.kind == AUDIO_KIND_SILENCE,
                "Tones cleanup retires its own ringtone preview");

    audio_service_command(CORE1_CMD_AUDIO_TONES_SYSTEM_PREVIEW, message);
    assert_true(s_tones_preview_owned &&
                    s_audio.kind == AUDIO_KIND_TONE_STREAM,
                "Tones message preview owns its system-tone stream");
    audio_service_command(CORE1_CMD_AUDIO_TONES_PREVIEW_STOP, 0u);
    assert_true(s_audio.kind == AUDIO_KIND_SILENCE,
                "Tones cleanup retires its own message preview");

    audio_service_command(CORE1_CMD_AUDIO_TONES_CLICK_PREVIEW, click);
    assert_true(s_tones_preview_owned && s_audio.kind == AUDIO_KIND_CLICK,
                "Tones keypad preview owns its click");
    audio_service_command(CORE1_CMD_AUDIO_TONES_PREVIEW_STOP, 0u);
    assert_true(s_audio.kind == AUDIO_KIND_SILENCE,
                "Tones cleanup retires its own keypad preview");

    audio_service_command(CORE1_CMD_AUDIO_RINGTONE_MENU_PREVIEW, ring);
    audio_service_command(CORE1_CMD_AUDIO_CLICK, click);
    audio_service_command(CORE1_CMD_AUDIO_TONES_PREVIEW_STOP, 0u);
    assert_true(!s_tones_preview_owned && s_audio.kind == AUDIO_KIND_CLICK,
                "late Tones cleanup preserves the queued navigation click");

    audio_service_command(CORE1_CMD_AUDIO_RINGTONE_MENU_PREVIEW, ring);
    audio_service_command(CORE1_CMD_AUDIO_RINGTONE_LOOP, ring);
    audio_service_command(CORE1_CMD_AUDIO_TONES_PREVIEW_STOP, 0u);
    assert_true(!s_tones_preview_owned &&
                    s_audio.kind == AUDIO_KIND_TONE_STREAM,
                "late Tones cleanup preserves a replacement incoming ring");
}

/* =========================================================================
 * composer_packed_decode: the Nokia composer packed-stream -> sequence-step
 * adapter. Build a real stream with the shared encoder (the same codec the
 * tones_app writes with), decode through the FILE-STATIC adapter, and check
 * the per-note timing/pitch mapping against first-principles oracles:
 *   duration = (60000 / bpm) * 4 / denom, dotted += half (composer_note_ms),
 *   pitch    = ROM G_NOTE_FREQ[62 + semitone + (octave-1)*12] (0x40-based),
 *   rest     = pitch_code 0 -> 0 Hz.
 * Tempo bytecode 0x0c is BPM_BY_TEMPO[8] = 100 BPM, so a quarter is 600 ms.
 * ========================================================================= */
static void test_composer_packed_decode_adapter(void) {
    static const composer_note_event_t notes[] = {
        {1u, 1u, 2u, false}, /* C, octave 1, quarter            -> 600 ms */
        {2u, 5u, 3u, true},  /* E, octave 2, dotted eighth      -> 450 ms */
        {1u, 0u, 0u, false}, /* rest, whole                     -> 2400 ms */
    };
    uint8_t packed[64];
    uint16_t packed_len = 0u;
    bool encoded = composer_codec_encode("Own", notes, 3u, 0x0cu,
                                         packed, sizeof(packed), &packed_len);
    assert_true(encoded && packed_len > 0u, "composer encoder produces a stream");
    if (!encoded) {
        return;
    }

    audio_sequence_step_t steps[8];
    memset(steps, 0xa5, sizeof(steps));
    uint8_t count = 0xffu;
    bool ok = composer_packed_decode(packed, packed_len, steps,
                                     (uint8_t)(sizeof(steps) / sizeof(steps[0])), &count);
    assert_true(ok, "packed decode of a valid 3-note stream succeeds");
    assert_true(count == 3u, "packed decode yields exactly 3 steps");
    if (!ok || count != 3u) {
        return;
    }
    /* Durations: 100 BPM -> beat 600 ms. */
    assert_true(steps[0].duration_ms == 600u, "quarter @100BPM = 600 ms");
    assert_true(steps[1].duration_ms == 450u, "dotted eighth @100BPM = 300 + 150 ms");
    assert_true(steps[2].duration_ms == 2400u, "whole @100BPM = 2400 ms");
    /* Pitches: octave/pitch -> ROM note-table index, rest -> silence. */
    assert_true(steps[2].hz == 0u, "rest (pitch_code 0) decodes to 0 Hz");
    assert_true(steps[0].hz > 0u, "C1 is voiced");
    assert_true(steps[1].hz > steps[0].hz, "E2 is higher than C1");
    /* Exact mapping through the ROM table: C1 -> index 62 (0x7e), E2 -> index
     * 62 + 4 + 12 = 78 (0x8e). Table-verified: 523 Hz and 1319 Hz. */
    assert_true(steps[0].hz == tone_frequency_hz(0x7eu), "C1 maps to ROM pitch 0x7e");
    assert_true(steps[1].hz == tone_frequency_hz(0x8eu), "E2 maps to ROM pitch 0x8e");
    assert_true(steps[0].hz == 523u, "C1 = 523 Hz (ROM G_NOTE_FREQ[62])");
    assert_true(steps[1].hz == 1319u, "E2 = 1319 Hz (ROM G_NOTE_FREQ[78])");

    /* A smaller step cap truncates the sequence (extra notes dropped, not an
     * error) and reports the truncated count. */
    memset(steps, 0xa5, sizeof(steps));
    count = 0xffu;
    ok = composer_packed_decode(packed, packed_len, steps, 2u, &count);
    assert_true(ok, "packed decode with cap 2 still succeeds");
    assert_true(count == 2u, "packed decode with cap 2 yields 2 steps");
    assert_true(steps[0].duration_ms == 600u && steps[1].duration_ms == 450u,
                "cap 2 keeps the first two notes in order");

    /* Corrupt framing (first header byte must be 0x02) is rejected outright. */
    uint8_t corrupt[64];
    memcpy(corrupt, packed, packed_len);
    corrupt[0] ^= 0xffu;
    count = 0xffu;
    ok = composer_packed_decode(corrupt, packed_len, steps,
                                (uint8_t)(sizeof(steps) / sizeof(steps[0])), &count);
    assert_true(!ok, "corrupted first byte is rejected");
    assert_true(count == 0u, "rejected stream reports zero steps");
}

int main(void) {
    test_disabled_modem_voice_transport_is_inert();
    test_composer_packed_decode_adapter();
    test_bridge_transport_failure_is_fail_closed();
    test_bridge_start_waits_for_fresh_bclk();
    test_bridge_start_retries_after_resync_failure();
    test_duration_samples_oracle();
    test_rest_note_0x40();
    test_note_0xa6_is_900hz();
    test_note_normal_pitch();
    test_dtmf_rest_0x04();
    test_dtmf_rest_0x04_truncated();
    test_dtmf_rest_0x04_key_only();
    test_crescendo_seed_0x09_armed();
    test_crescendo_seed_0x09_not_armed_without_dynamics();
    test_crescendo_seed_0x09_low_ceiling();
    test_alarm_crescendo_uses_five_low_to_full_levels();
    test_ringtone_level5_preview_envelope_is_mode_scoped();
    test_marker_0x0a_light_and_vibra();
    test_marker_0x0a_truncated();
    test_marker_0x0a_vibra_gated();
    test_repeat_block_0x05_0x06();
    test_repeat_count_zero();
    test_repeat_nesting_overflow();
    test_repeat_0x05_truncated();
    test_restart_finite_0x07_0x0b();
    test_restart_forever();
    test_one_operand_opcodes();
    test_one_operand_opcode_truncated();
    test_noop_opcodes();
    test_truncation_sweep_generated_ringtone();
    test_truncation_sweep_all_resources();
    test_square_stream_headset_duplication();
    test_garbage_fuzz();
    test_audio_fill_integration();
    test_headset_local_gain_does_not_attenuate_voice();
    test_keypad_audio_calibration_controls();
    test_tone_start_offset_cases();
    test_decoder_empty_and_null();
    test_buzzer_command_routing_contract();
    test_call_tones_have_isolated_quiet_gain();
    test_debug_output_ownership();
    test_marker_vibra_is_owned_by_stream_start();
    test_composer_preview_stop_is_owner_scoped();
    test_composer_navigation_stop_is_owner_scoped();
    test_tones_navigation_stop_is_owner_scoped();

    if (s_failures != 0) {
        fprintf(stderr, "%d failures\n", s_failures);
        return 1;
    }
    printf("audio_tonedecode tests passed\n");
    return 0;
}
