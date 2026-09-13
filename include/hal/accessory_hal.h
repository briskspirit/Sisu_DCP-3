#ifndef ACCESSORY_HAL_H
#define ACCESSORY_HAL_H

#include <stdbool.h>
#include <stdint.h>

/* Headset / accessory detection for the Sisu DCP-3 (Nokia bottom connector J2).
 * Insert is HEAD_INT on the TCA8418 ROW7 GPI. The empty connector's normally
 * closed contact references it to X_EAR_P; insertion opens that contact and the
 * external pull-up wins, so LOW=empty and HIGH=inserted. The HDC-5 inline
 * answer/end button shorts the headset mic line, read as an analog level on
 * GP41/ADC1 (collapses toward 0 V when pressed). There is no car-kit connector,
 * so only the headset accessory class exists. Core 0 only (shares the ADC with
 * battery_hal and the I2C with the keypad). */

void accessory_hal_init(void);
void accessory_hal_poll(uint32_t now_ms);

bool accessory_hal_headset_inserted(void);

/* True once per debounced insert/remove edge (consume-on-read); *now_inserted (if
 * non-NULL) gets the new state. The caller activates/restores the headset profile. */
bool accessory_hal_take_insert_change(bool *now_inserted);

/* True once per debounced hook-button press (consume-on-read). Maps to the HDC-5
 * answer/end button: the caller turns it into answer-on-ring / hang-up-in-call. */
bool accessory_hal_take_hook_press(void);

/* Last sampled hook-line voltage in mV (diagnostics / bench threshold tuning). */
uint16_t accessory_hal_hook_mv(void);

/* Live debounced hook-press state (for diagnostics / Net Monitor -- unlike
 * accessory_hal_take_hook_press() this does not consume the edge). */
bool accessory_hal_hook_pressed(void);

/* False while an insert/remove or hook transition still needs polling or has a
 * consume-on-read edge pending. Dormant standby must not clear the shared IRQ
 * and suspend before the longer accessory debounce can publish that edge. */
bool accessory_hal_standby_ready(void);

typedef enum {
    ACCESSORY_DEBUG_OVERRIDE_AUTO = 0,
    ACCESSORY_DEBUG_OVERRIDE_INSERTED,
    ACCESSORY_DEBUG_OVERRIDE_REMOVED,
} accessory_debug_override_t;

/* Diagnostic override. Changing the effective state emits the same deferred
 * insert-change edge as hardware, so profile and live-call routing remain owned
 * by the normal main-loop reconciliation path. */
void accessory_hal_debug_set_override(accessory_debug_override_t override);
accessory_debug_override_t accessory_hal_debug_get_override(void);

#endif
