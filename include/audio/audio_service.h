#ifndef AUDIO_SERVICE_H
#define AUDIO_SERVICE_H

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    bool voice_transport_available;
    bool voice_transport_running;
    bool bridge_start_pending;
    bool bclk_qualifying;
    bool bridge_active;
    uint32_t bridge_start_requests;
    uint32_t bridge_activations;
    uint32_t bridge_stop_requests;
    uint32_t bridge_bclk_losses;
    uint32_t bridge_resync_failures;
    uint32_t bridge_last_acquire_ms;
    uint32_t bridge_max_acquire_ms;
    uint32_t bridge_last_request_ms;
    uint32_t bridge_last_active_ms;
    uint32_t bridge_last_stop_ms;
} audio_service_diag_t;

/* `modem_voice_transport_available=false` leaves the modem-side I2S HAL fully
 * inert while retaining local codec audio. */
void audio_service_init(bool modem_voice_transport_available);
void audio_service_tick(uint32_t now_ms);
void audio_service_command(uint16_t command, uint16_t arg);
/* Pure routing predicate shared with core0's +3V8 owner. It must stay aligned
 * with audio_service_command(): true means this command drives the magnetic
 * buzzer rather than only the codec/earpiece. */
bool audio_service_command_uses_buzzer(uint16_t command, uint16_t arg);
void audio_service_start_composer_packed(const uint8_t *data, uint16_t len, uint8_t level);
/* Looping variant: replays the packed melody until stopped (own-tone ring). */
void audio_service_start_composer_packed_loop(const uint8_t *data, uint16_t len, uint8_t level);
void audio_service_start_packed_tone_preview(const uint8_t *data, uint16_t len, uint8_t level);

/* Activate/deactivate the modem<->codec voice bridge (core 1; dispatched from
 * a core1 command). The codec route + format flags are set on core 0 first. */
void audio_service_bridge_start(void);
void audio_service_bridge_stop(void);

/* True while core 1 is producing audible output (tone/DTMF/click/ringtone/
 * composer, a vibra pulse, or an active voice bridge). Core 0 reads this to
 * avoid starting a flash commit (which parks core 1) mid-playback or mid-call.
 * Advisory, lock-free. */
bool audio_service_is_active(void);

/* M7: true while a ringtone light marker (tone op 0x0a operand 0x01) is on.
 * Core 0 mirrors this onto the backlight to flash it in sync with the ring.
 * Advisory, lock-free. */
bool audio_service_ring_light(void);
void audio_service_get_diag(audio_service_diag_t *out);

#endif
