#ifndef NAU88C22_CODEC_H
#define NAU88C22_CODEC_H

#include <stdbool.h>
#include <stdint.h>

#define NAU88C22_I2C_ADDR 0x1au

/* Audio routes. Value matches the route field published to core1 via
 * audio_bridge_set_format(). */
typedef enum {
    NAU_ROUTE_HANDSET = 0,     /* BTL receiver + internal left mic */
    NAU_ROUTE_LOUDSPEAKER = 1, /* BTL speaker (gain profile of handset for now) */
    NAU_ROUTE_HEADSET = 2,     /* differential headphone + right mic */
} nau_route_t;

typedef struct {
    bool ready;
    bool recover_pending;
    nau_route_t route;
    bool mic_in_call;
    bool mic_bias_hold;
    bool playback_idle;
    uint8_t speaker_gain;
    bool speaker_muted;
    uint8_t headphone_gain;
    bool headphone_muted;
} nau88c22_codec_diag_t;

/* Low-level initialization requires a stable 6.144 MHz MCLK throughout both
 * depop delays. Production callers use core1_services_codec_init(), which also
 * reconciles the audio-idle gate; hardware diagnostics may call this directly
 * only after establishing MCLK themselves. */
bool nau88c22_codec_init(void);
bool nau88c22_codec_ready(void);
void nau88c22_codec_get_diag(nau88c22_codec_diag_t *out);

/* True while a runtime I2C bus-wedge latch is armed and awaiting recovery. The
 * core1_services recovery tick drives the actual re-init (it owns MCLK / the
 * clock-down exit / the idle-gate state that recovery must reconcile). */
bool nau88c22_codec_recover_pending(void);
/* Re-arm (or clear) the wedge-recovery retry. Used by the recovery tick to keep
 * recovery pending when the post-init route/mic reconcile itself NAK'd (a single
 * fault won't re-trip the 8-fail latch), so the next tick retries. */
void nau88c22_codec_set_recover_pending(bool pending);

/* Switch the analog input/output route. Core 0 only (shared I2C bus). Mutes
 * around the change, reconfigures power/mixers/mic, and publishes the matching
 * sample-format flags to core1 for the bridge mixer. */
bool nau88c22_codec_set_route(nau_route_t route);

/* Mic-chain gate: gate the mic chain (MICBIAS + boost/PGA/ADC, ~3.8 mA of VDDA)
 * to calls only. in_call powers the current route's chain; headset_inserted
 * alone keeps just MICBIAS up (the HDC-5 hook comparator hangs off the mic
 * bias rail -- see accessory_hal). Callers: the modem bridge follower at call
 * start/end, modem_service_accessory_changed on insert/remove, and init's
 * idle default. Core 0 only. State is cached when the codec is down and
 * derived into POWER1/POWER2 by the next init/set_route. [BP] bench: mic
 * settle after enable must land before uplink starts (expected <10 ms,
 * hidden in call setup); hook sense with a headset present from boot. */
bool nau88c22_codec_set_mic_power(bool in_call, bool headset_inserted);

/* Playback idle: playback light-sleep for the audio idle gate. idle=true mutes
 * the live output then drops the DACs+mixers behind it (~3-4 mA); the
 * speaker/HP DRIVER enables always stay set -- re-enabling a driver triggers
 * the chip's 250 ms depop sequence, which must never land on the keypad-
 * click path. idle=false restores DACs first, then unmutes. Core 0 only;
 * driven by core1_services' audio gate. [BP] bench: DAC-toggle audibility. */
bool nau88c22_codec_set_playback_idle(bool idle);

bool nau88c22_codec_read_reg(uint8_t reg, uint16_t *out_value);
bool nau88c22_codec_write_reg(uint8_t reg, uint16_t value);

bool nau88c22_codec_set_speaker_gain(uint8_t gain);
/* Headphone (headset) analog gain, same 0x00..0x3f scale as the speaker. Cached
 * across route changes: set_route(HEADSET) unmutes to the cached value, so a
 * mid-call insert picks up the current call volume automatically. Core 0 only. */
bool nau88c22_codec_set_headphone_gain(uint8_t gain);
/* Apply the semantic Nokia 1..10 call-volume level to both cached output
 * routes. The live route changes immediately; the other cache makes a
 * mid-call headset insert/remove inherit the same user-selected level. */
bool nau88c22_codec_set_call_volume(uint8_t level);
/* Restore BOTH output gains to their tone-path defaults. Call at call end: the
 * in-call volume keys move these same analog stages, and tones' digital level
 * tables assume the defaults -- without the restore, every tone after a quiet
 * call plays quieter than intended. Core 0 only. */
bool nau88c22_codec_reset_output_gains(void);

/* Phone-off analog shutdown: mute + POWER1/2/3 = 0 (bias, mic bias, ADC/DAC,
 * mixers, drivers all off), then clears the ready latch so every codec API
 * early-returns until resume -- which is nau88c22_codec_init() again (the latch
 * gates its short-circuit; full re-init, ~500 ms, blocking sleeps). Used by
 * power_off(); power-on and the alarm-while-off wake re-init. Core 0 only. */
bool nau88c22_codec_power_standby(void);

#endif
