#include "hal/accessory_hal.h"

#include "hardware/adc.h"
#include "hardware/gpio.h"
#include "hal/board.h"
#include "hal/tca8418_hal.h"
#include "services/timebase.h"

#define ADC_FULL_SCALE 4095u
#define ADC_REF_MV 3300u
#define ACCESSORY_INSERT_POLL_MS 100u
#define ACCESSORY_HOOK_POLL_MS 30u
#define HOOK_DEBOUNCE_MS 40u
#define INSERT_DEBOUNCE_MS 80u  /* longer: ride out connector bounce on plug/unplug */
/* Ignore insert readings for this long after boot: the TCA input path can serve
 * garbage for several seconds on a battery-insert cold boot (see poll). */
#define ACCESSORY_BOOT_QUARANTINE_MS 10000u

/* Rev B2 board-1 bench bands (2026-08-10): inserted/open ~=2540 mV and
 * pressed ~=1510 mV. Use a 1900/2200 mV Schmitt window around their midpoint:
 * enough margin for connector/mic variation without allowing a held press to
 * chatter near one threshold. Net Monitor page 46 exposes the live value. */
#define HOOK_PRESS_MV 1900u
#define HOOK_RELEASE_MV 2200u
/* De-flake the hook read: discard a few conversions after channel selection,
 * then average. GP41 shares the ADC with the Rev B2 charger-input and, where
 * fitted, modem-status observations, so each consumer settles its own channel. */
#define HOOK_ADC_SETTLE 4u
#define HOOK_ADC_BURST 8u

static bool s_inserted;             /* debounced insert state */
static bool s_insert_change_pending;
static uint32_t s_insert_change_ms;
static bool s_insert_edge_pending;  /* latched insert/remove edge, consumed by take_insert_change */
static uint16_t s_hook_mv;
static bool s_hook_raw;        /* instantaneous pressed sample */
static bool s_hook_debounced;  /* debounced pressed state */
static bool s_hook_change_pending;
static uint32_t s_hook_change_ms;
static bool s_hook_press_pending; /* latched rising edge, consumed by take_hook_press */
static uint32_t s_last_insert_poll_ms;
static uint32_t s_last_hook_poll_ms;
static bool s_insert_polled;
static bool s_hook_polled;
static bool s_quarantine_over; /* latched once the boot-settle window has passed
                                * (replaces an absolute-constant time compare that
                                * re-engaged on the 32-bit ms half-wrap) */
static accessory_debug_override_t s_debug_override;

static bool effective_inserted(void) {
    if (s_debug_override == ACCESSORY_DEBUG_OVERRIDE_INSERTED) {
        return true;
    }
    if (s_debug_override == ACCESSORY_DEBUG_OVERRIDE_REMOVED) {
        return false;
    }
    return s_inserted;
}

void accessory_hal_init(void) {
    adc_init();
    adc_gpio_init(HEADSET_HOOK_ADC_PIN);
    gpio_set_input_enabled(HEADSET_HOOK_ADC_PIN, false);
    s_inserted = false;
    s_insert_change_pending = false;
    s_insert_edge_pending = false;
    s_hook_mv = ADC_REF_MV;
    s_hook_raw = false;
    s_hook_debounced = false;
    s_hook_change_pending = false;
    s_hook_press_pending = false;
    s_last_insert_poll_ms = 0u;
    s_last_hook_poll_ms = 0u;
    s_insert_polled = false;
    s_hook_polled = false;
    s_quarantine_over = false;
    s_debug_override = ACCESSORY_DEBUG_OVERRIDE_AUTO;
    accessory_hal_poll(0u);
}

void accessory_hal_poll(uint32_t now_ms) {
    bool insert_due = !s_insert_polled ||
        time_diff_ms(
            now_ms,
            s_last_insert_poll_ms + ACCESSORY_INSERT_POLL_MS) >= 0;
    if (insert_due) {
        s_insert_polled = true;
        s_last_insert_poll_ms = now_ms;
        bool level = false;
        if (tca8418_hal_input_level(TCA8418_PIN_ROW7_HEAD_INT, &level)) {
            /* HEAD_INT is a normally-closed contact in J2: EMPTY ties it to
             * X_EAR_P (which R62 100k references to GND), while INSERTING the
             * plug lifts the contact and the external 470k pull-up wins. Raw
             * LOW = empty, raw HIGH = inserted. The TCA internal pull-up must
             * stay disabled or it skews the divider high.
             *
             * Keep the cold-boot quarantine: a settling TCA can temporarily
             * serve all-zero GPIO reads. Latch its completion instead of using
             * a signed comparison against an absolute constant, which would
             * re-enter quarantine near the 32-bit millisecond half-wrap. */
            if (!s_quarantine_over && now_ms >= ACCESSORY_BOOT_QUARANTINE_MS) {
                s_quarantine_over = true;
            }
            if (!s_quarantine_over) {
                s_insert_change_pending = false;
            } else if (level != s_inserted) {
                if (!s_insert_change_pending) {
                    s_insert_change_pending = true;
                    s_insert_change_ms = now_ms;
                } else if (time_diff_ms(
                               now_ms,
                               s_insert_change_ms + INSERT_DEBOUNCE_MS) >= 0) {
                    s_inserted = level;
                    s_insert_change_pending = false;
                    s_insert_edge_pending = true;
                }
            } else {
                s_insert_change_pending = false;
            }
        }
    }

    bool inserted = effective_inserted();
    if (!inserted) {
        /* The hook line is meaningless with no headset and may sit low while
         * MICBIAS is off. Avoid twelve needless ADC conversions every 30 ms and
         * retire any half-debounced press before the next insertion. */
        s_hook_mv = ADC_REF_MV;
        s_hook_raw = false;
        s_hook_debounced = false;
        s_hook_change_pending = false;
        s_hook_press_pending = false;
        s_hook_polled = false;
        return;
    }
    if (s_hook_polled &&
        time_diff_ms(
            now_ms,
            s_last_hook_poll_ms + ACCESSORY_HOOK_POLL_MS) < 0) {
        return;
    }
    s_hook_polled = true;
    s_last_hook_poll_ms = now_ms;

    adc_select_input(HEADSET_HOOK_ADC_INPUT);
    for (uint32_t i = 0; i < HOOK_ADC_SETTLE; i++) {
        (void)adc_read(); /* discard: the ADC is muxed with the battery channel */
    }
    uint32_t acc = 0u;
    for (uint32_t i = 0; i < HOOK_ADC_BURST; i++) {
        acc += adc_read();
    }
    uint16_t raw = (uint16_t)(acc / HOOK_ADC_BURST);
    s_hook_mv = (uint16_t)(((uint32_t)raw * ADC_REF_MV + (ADC_FULL_SCALE / 2u)) / ADC_FULL_SCALE);

    /* The button is only meaningful with a headset inserted. With MICBIAS off,
     * an empty connector may itself read low, so insertion is a mandatory gate,
     * not merely a plausibility check. Use the EFFECTIVE inserted state
     * (physical OR the Net Monitor force override) so the manual-headset bench
     * mode gets a working button too. */
    if (s_hook_raw) {
        s_hook_raw = s_hook_mv < HOOK_RELEASE_MV;
    } else {
        s_hook_raw = s_hook_mv < HOOK_PRESS_MV;
    }

    /* Time-based debounce: a new level must hold HOOK_DEBOUNCE_MS before it is
     * accepted; a rising edge (-> pressed) latches one press. */
    if (s_hook_raw != s_hook_debounced) {
        if (!s_hook_change_pending) {
            s_hook_change_pending = true;
            s_hook_change_ms = now_ms;
        } else if (time_diff_ms(now_ms, s_hook_change_ms + HOOK_DEBOUNCE_MS) >= 0) {
            s_hook_debounced = s_hook_raw;
            s_hook_change_pending = false;
            if (s_hook_debounced) {
                s_hook_press_pending = true;
            }
        }
    } else {
        s_hook_change_pending = false;
    }
}

void accessory_hal_debug_set_override(accessory_debug_override_t override) {
    if (override > ACCESSORY_DEBUG_OVERRIDE_REMOVED) {
        override = ACCESSORY_DEBUG_OVERRIDE_AUTO;
    }
    bool before = effective_inserted();
    s_debug_override = override;
    bool after = effective_inserted();
    if (before != after) {
        s_insert_edge_pending = true;
    }
}

accessory_debug_override_t accessory_hal_debug_get_override(void) {
    return s_debug_override;
}

bool accessory_hal_headset_inserted(void) {
    return effective_inserted();
}

bool accessory_hal_take_hook_press(void) {
    bool pressed = s_hook_press_pending;
    s_hook_press_pending = false;
    return pressed;
}

bool accessory_hal_take_insert_change(bool *now_inserted) {
    bool changed = s_insert_edge_pending;
    s_insert_edge_pending = false;
    if (now_inserted != 0) {
        *now_inserted = effective_inserted();
    }
    return changed;
}

uint16_t accessory_hal_hook_mv(void) {
    return s_hook_mv;
}

bool accessory_hal_hook_pressed(void) {
    return s_hook_debounced; /* live debounced press state (diagnostics / Net Monitor) */
}

bool accessory_hal_standby_ready(void) {
    return !s_insert_change_pending && !s_insert_edge_pending &&
           !s_hook_change_pending && !s_hook_press_pending;
}
