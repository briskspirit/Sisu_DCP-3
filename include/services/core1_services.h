#ifndef CORE1_SERVICES_H
#define CORE1_SERVICES_H

#include <stdbool.h>
#include <stdint.h>

#define CORE1_AUDIO_COMPOSER_PACKED_MAX 256u

typedef enum {
    CORE1_AUDIO_GAME_LEVEL_STEP = 0,
    CORE1_AUDIO_GAME_LEVEL_UPPER_LIMIT,
    CORE1_AUDIO_GAME_LEVEL_LOWER_LIMIT,
} core1_audio_game_level_tone_t;

typedef enum {
    CORE1_CMD_NONE = 0,
    CORE1_CMD_AUDIO_CLICK = 1,
    CORE1_CMD_AUDIO_STOP = 2,
    CORE1_CMD_AUDIO_SYSTEM_TONE_QUIET_LOOP = 3,
    CORE1_CMD_AUDIO_DTMF = 4,
    CORE1_CMD_AUDIO_SYSTEM_TONE = 5,
    CORE1_CMD_AUDIO_RINGTONE_PREVIEW = 6,
    CORE1_CMD_AUDIO_RINGTONE_LOOP = 7,
    CORE1_CMD_RESERVED_8 = 8,
    CORE1_CMD_RESERVED_9 = 9,
    CORE1_CMD_AUDIO_SYSTEM_TONE_LOOP = 10,
    CORE1_CMD_AUDIO_PACMAN_TONE = 11,
    CORE1_CMD_AUDIO_COMPOSER_NOTE = 12,
    CORE1_CMD_AUDIO_COMPOSER_PACKED = 13,
    /* Fixed low-gain earpiece path for call-progress/notification tones. It
     * deliberately sits below the user-facing level-1 floor without changing
     * keypad or ordinary system-tone loudness globally. */
    CORE1_CMD_AUDIO_SYSTEM_TONE_QUIET = 14,
    CORE1_CMD_AUDIO_VIBRA_PULSE = 15,
    CORE1_CMD_AUDIO_VIBRA_PULSE_LOOP = 16,
    CORE1_CMD_AUDIO_RINGTONE_MENU_PREVIEW = 17,
    CORE1_CMD_AUDIO_BRIDGE_START = 18,
    CORE1_CMD_AUDIO_BRIDGE_STOP = 19,
    CORE1_CMD_AUDIO_COMPOSER_PACKED_LOOP = 20,
    CORE1_CMD_AUDIO_SET_EARPIECE_GAIN = 21, /* arg = Q8 gain (256 = unity); bench trim */
    CORE1_CMD_SHUTDOWN = 22,                /* POWMAN power-off: silence, quiesce I2S, park for good */
    CORE1_CMD_AUDIO_GATE = 23,   /* Audio idle gate: park the codec I2S if audio is idle */
    CORE1_CMD_AUDIO_UNGATE = 24, /* Audio idle gate: explicit resume (augate off / safety) */
    CORE1_CMD_AUDIO_DEBUG_BUZZER_TEST = 25,
    CORE1_CMD_AUDIO_DEBUG_VIBRA_TEST = 26,
    CORE1_CMD_AUDIO_DEBUG_STOP = 27,
    /* Stop only a still-current Composer key preview. A delayed preview
     * cleanup must never stop a ringtone, alarm, or call-progress tone. */
    CORE1_CMD_AUDIO_COMPOSER_PREVIEW_STOP = 28,
    /* Immediate editor navigation cleanup. Stops a Composer preview or packed
     * playback, but preserves the keypad click queued before the app handler
     * and any newer non-Composer audio owner. */
    CORE1_CMD_AUDIO_COMPOSER_STOP = 29,
    /* Volatile bench overrides. These commands update core1 synth parameters
     * in RAM; reboot (or `keycal reset`) restores the measured production
     * defaults from audio_levels.h. */
    CORE1_CMD_AUDIO_SET_CLICK_GAIN = 30,      /* arg = Q8 gain, 256 = unity */
    CORE1_CMD_AUDIO_SET_CLICK_H3 = 31,        /* arg = signed Q12 3rd harmonic */
    CORE1_CMD_AUDIO_SET_CLICK_RELEASE_MS = 32,
    CORE1_CMD_AUDIO_SET_DTMF_GAIN = 33,       /* arg = Q8 gain, 256 = unity */
    CORE1_CMD_AUDIO_SET_DTMF_WEIGHTS = 34,    /* arg = high:low, one byte each */
    CORE1_CMD_AUDIO_SET_CLICK_H3_PHASE = 35,  /* arg = signed degrees */
    CORE1_CMD_AUDIO_SET_CLICK_ATTACK_MS = 36,
    CORE1_CMD_AUDIO_SET_CLICK_H2 = 37,        /* arg = signed Q12 harmonic */
    CORE1_CMD_AUDIO_SET_CLICK_H2_PHASE = 38,  /* arg = signed degrees */
    CORE1_CMD_AUDIO_SET_CLICK_H4 = 39,        /* arg = signed Q12 harmonic */
    CORE1_CMD_AUDIO_SET_CLICK_H4_PHASE = 40,  /* arg = signed degrees */
    CORE1_CMD_AUDIO_SET_CLICK_H5 = 41,        /* arg = signed Q12 harmonic */
    CORE1_CMD_AUDIO_SET_CLICK_H5_PHASE = 42,  /* arg = signed degrees */
    /* Volatile buzzer calibration: arg 0..50 = exact PWM duty percent;
     * UINT16_MAX restores the production level curve. */
    CORE1_CMD_AUDIO_SET_BUZZER_DUTY = 43,
    CORE1_CMD_AUDIO_SET_DTMF_HEADSET_WEIGHTS = 44, /* arg = high:low */
    /* Tones value-picker previews have their own cleanup owner. Main queues the
     * physical key click before dispatching the UI event, so a later picker
     * cleanup must preserve that click while still stopping an older preview. */
    CORE1_CMD_AUDIO_TONES_SYSTEM_PREVIEW = 45,
    CORE1_CMD_AUDIO_TONES_CLICK_PREVIEW = 46,
    CORE1_CMD_AUDIO_TONES_PREVIEW_STOP = 47,
    /* Stock game-level feedback. The low argument byte selects step / upper /
     * lower; the high byte remains the active profile's warning-tone level. */
    CORE1_CMD_AUDIO_GAME_LEVEL_TONE = 48,
    CORE1_CMD_AUDIO_PACKED_TONE_PREVIEW = 49,
} core1_cmd_t;

typedef struct {
    bool started;
    bool command_dispatching;
    bool idle_waiting;
    bool flash_pause_requested;
    bool flash_parked;
    bool audio_gate_enabled;
    bool audio_gated;
    uint8_t command_queue_depth;
    uint8_t command_queue_high_water;
    uint32_t command_queue_drops;
    uint32_t heartbeat_ms;
    uint32_t loop_count;
    uint32_t flash_pause_attempts;
    uint32_t flash_pause_successes;
    uint32_t flash_pause_timeouts;
    uint32_t flash_resume_timeouts;
    uint32_t flash_park_entries;
    uint32_t flash_park_last_flags;
    uint32_t audio_gate_requests;
    uint32_t audio_gate_successes;
    uint32_t audio_gate_races;
    uint32_t audio_resume_count;
    uint32_t codec_recovery_attempts;
    uint32_t codec_recovery_successes;
    uint32_t codec_recovery_failures;
    uint32_t bridge_start_posts;
    uint32_t bridge_stop_posts;
    uint32_t bridge_last_dispatch_ms;
    uint32_t bridge_max_dispatch_ms;
} core1_services_diag_t;

/* flash_park_last_flags: a lock-free evidence snapshot written by core1 before
 * it either acknowledges or rejects the most recent flash-park request. */
#define CORE1_FLASH_PARK_AUDIO_GATED (1u << 0)
#define CORE1_FLASH_PARK_MODEM_RUNNING (1u << 1)
#define CORE1_FLASH_PARK_CODEC_FAIL (1u << 2)
#define CORE1_FLASH_PARK_MODEM_FAIL (1u << 3)
#define CORE1_FLASH_PARK_ACKED (1u << 4)

/* `modem_voice_transport_available` is a backend capability captured before
 * core1 launches. False keeps modem-side PIO/DMA and signal pads untouched. */
void core1_services_start(bool modem_voice_transport_available);

/* POWMAN power-off: post CORE1_CMD_SHUTDOWN and wait (bounded) for core1 to
 * acknowledge that it has silenced audio, quiesced the I2S DMA/PIO plumbing,
 * and parked for good. Nothing resumes until the wake reboot, so callers must
 * only invoke this once the switched-core power-off is committed -- in
 * particular AFTER the final storage flush (flash commits need the core1 park
 * handshake, which a shut-down core1 no longer answers; begin() then times out
 * and the commit is skipped). Returns false on ack timeout (core1 wedged);
 * the power-off may proceed regardless -- the domain is about to lose power. */
bool core1_services_shutdown(uint32_t timeout_ms);

/* Flash-write coordination (see core1_services.c). The storage HAL calls these
 * around a flash erase/program: core0 parks core1 in a RAM-resident spin (IRQs
 * off) so the audio core is not executing from XIP during the op, then releases
 * it. Replaces the SDK's flash_safe_execute() FIFO-IRQ lockout, which the
 * audio-resident core1 does not reliably acknowledge.
 * begin() returns false if core1 could not be parked in time (caller must then
 * skip the flash op and retry later). Safe before core1 launches (no-op). */
bool core1_flash_pause_begin(void);
void core1_flash_pause_end(void);

void core1_post_command(core1_cmd_t cmd, uint16_t arg);
void core1_post_audio_composer_packed(const uint8_t *data, uint16_t len, uint8_t level);
/* Looping variant (own-tone ring): replays until CORE1_CMD_AUDIO_STOP. */
void core1_post_audio_composer_packed_loop(const uint8_t *data, uint16_t len, uint8_t level);
void core1_post_audio_packed_tone_preview(const uint8_t *data, uint16_t len, uint8_t level);

/* Audio idle gate -- the audio idle gate. During silence the codec I2S PIO+DMA
 * ping-pong is parked (the SAME battle-tested quiesce/resume pair the
 * flash-park path uses; the modem duplex side is never touched), the codec
 * DACs/mixers sleep behind the analog mutes, and the 6.144 MHz MCLK gpout
 * stops. core0 orchestrates via core1_services_audio_gate_tick() (call each
 * main-loop tick); ANY sound-starting post transparently un-idles the codec
 * before it is queued, and core1 resumes the I2S before dispatching it, so
 * no audio command can ever run against parked plumbing. Default ON (pop-tests
 * passed 2026-07-06); toggled by the `augate` console command. */
void core1_services_audio_gate_set_enabled(bool enabled);
bool core1_services_audio_gate_enabled(void);
bool core1_services_audio_gated(void); /* core1-published: I2S currently parked */
void core1_services_audio_gate_tick(uint32_t now_ms);
/* Strict powered-on dormant precondition. It is true only after every queued
 * command drained, both I2S transports are idle, and the codec gate completed. */
bool core1_services_standby_ready(void);

/* Production codec bring-up contract. Restores the full system clock, starts
 * the 6.144 MHz codec MCLK before the NAU88C22 depop delays, then reconciles
 * the audio-idle gate with the newly live codec. Safe before core1 launches;
 * core 0 only. Use this instead of calling nau88c22_codec_init() directly from
 * application/runtime code. */
bool core1_services_codec_init(void);

/* Codec analog standby for the soft-off idle (bias/drivers are a steady
 * multi-mA drain). Counterpart of core1_services_codec_init; core 0 only. */
void core1_services_codec_standby(void);

/* In-call volume using the Nokia 1..10 semantic level. Both output stages are
 * updated: whichever route is live changes immediately, and a mid-call
 * headset insert/remove picks up the matching cached route gain. Core 0 only
 * (codec I2C). */
void core1_services_codec_set_call_volume(uint8_t level);

/* Idempotently end the semantic call-volume session and restore both output
 * stages to their calibrated tone-path baselines. This also clears the
 * recovery latch for the selected call level, so a later codec re-init cannot
 * resurrect an ended call's attenuation. Core 0 only. */
void core1_services_codec_reset_call_volume(void);

/* core0 main-loop tick: self-heal the codec after a runtime I2C bus wedge once
 * the shared bus recovers. No-op unless a wedge is latched. The caller gates it
 * off in the power-off (soft-off) route so recovery never re-powers the codec. */
void core1_services_codec_recover_tick(uint32_t now_ms);
void core1_services_get_diag(core1_services_diag_t *out);

#endif
