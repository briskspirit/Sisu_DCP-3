#ifndef BUZZER_HAL_H
#define BUZZER_HAL_H

#include <stdbool.h>
#include <stdint.h>

/* Driver for the magnetic ring buzzer (CMT-1085-85-SMT, an external-drive
 * transducer). On the original 3210 the incoming-ring melody is played on a
 * separate magnetic buzzer driven by the ASIC PWM. The clone keeps that split:
 * this HAL drives the buzzer at each note's frequency, while earpiece-class
 * sounds use the I2S path. Driven from core1.
 *
 * buzzer_hal_available() reports whether a real pin is configured; when it is
 * not, the audio engine falls back to playing the ring on the I2S speaker so
 * the ring is still audible during bring-up. */
/* Drive level for the ascending ring. Controlled PodMic sweeps on Rev B2 found
 * the acoustic maximum near 24% duty; output falls again toward 50%. Audio
 * levels 1..5 map through the even drive levels to the measured stock-relative
 * ladder below. */
#define BUZZER_LEVEL_MAX 10u
#define BUZZER_DUTY_AUDIO_LEVEL1_PERCENT 2u
#define BUZZER_DUTY_AUDIO_LEVEL2_PERCENT 3u
#define BUZZER_DUTY_AUDIO_LEVEL3_PERCENT 5u
#define BUZZER_DUTY_AUDIO_LEVEL4_PERCENT 8u
#define BUZZER_DUTY_AUDIO_LEVEL5_PERCENT 24u

void buzzer_hal_init(void);
bool buzzer_hal_available(void);
void buzzer_hal_set_freq(uint16_t hz); /* hz == 0 silences the buzzer */
void buzzer_hal_set_level(uint8_t level); /* 0..BUZZER_LEVEL_MAX drive level; default MAX */
/* Volatile bench override. 0..50 forces that exact PWM duty for every level;
 * UINT8_MAX restores the production curve. It is reset by buzzer_hal_init(). */
void buzzer_hal_debug_set_duty_percent(uint8_t percent);
void buzzer_hal_off(void);

#endif
