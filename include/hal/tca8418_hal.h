#ifndef TCA8418_HAL_H
#define TCA8418_HAL_H

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    TCA8418_PIN_ROW7_HEAD_INT,
    TCA8418_PIN_COL3_CHR_STAT1,
    TCA8418_PIN_COL4_CHR_STAT2,
} tca8418_input_t;

typedef struct {
    bool transport_ok;
    bool did_work;
    bool more_work;
    uint8_t int_status;
    uint8_t gpi_status[3];
    uint8_t key_events;
} tca8418_irq_result_t;

bool tca8418_hal_init(void);
/* Returns the current pressed-state bitmap. A press observed in the hardware
 * FIFO is retained for one scan even if its release was already queued, so a
 * quick tap that wakes dormant standby cannot collapse to an empty state. */
uint16_t tca8418_hal_key_state(void);
/* Service one TCA contribution to the wired-OR SYS_INT line. GPI evidence is
 * copied before the clear-on-read registers are consumed, and key events are
 * applied to the cached keypad state before INT_STAT is cleared. Normal mode
 * keeps ROW7 HEAD_INT and both BQ25171 STAT pins armed for their opposite
 * levels, so either transition can wake powered-on dormant standby without
 * disabling keys. */
void tca8418_hal_service_interrupt(tca8418_irq_result_t *out);
bool tca8418_hal_input_level(tca8418_input_t input, bool *level);
/* STAT1/STAT2 share GPIO_DAT_STAT2. The TCA requires two reads for debounced
 * GPIO data; both outputs come from the same second byte so the charger policy
 * cannot observe a torn pair. */
bool tca8418_hal_charger_status(bool *stat1_high, bool *stat2_high);
/* Consume one latched COL3/COL4 interrupt observation. This survives whichever
 * path drained the TCA (shared-IRQ service or keypad safety poll), allowing the
 * battery HAL to force its slower status/ADC schedule immediately. */
bool tca8418_hal_take_charger_status_change(void);
/* BQ25171 /CE is active-low on COL5. Both calls verify that COL5 is an output,
 * its POR pull-up is disabled, and the output latch has the requested level.
 * A false return means the expander's charger-control state is not trustworthy;
 * register readback cannot prove the voltage at the BQ pin. */
bool tca8418_hal_set_charger_enabled(bool enabled);
bool tca8418_hal_get_charger_enabled(bool *enabled);
bool tca8418_hal_set_charger_li_ion_mode(bool enabled);

/* Reconfigure the TCA for the powered-off (dormant) window: keypad interrupts
 * off (keys must not wake an off phone) and the COL4 CHR_STAT2 falling-level
 * wake armed. It latches GPIO_INT_STAT and holds /INT (SYS_INT, GP42) low.
 * Service VBUS and modem-PWM are direct RP pins on Rev B2 and are deliberately
 * absent from this interface. */
bool tca8418_hal_arm_charger_wake_interrupt(void);

/* Debug: raw register access for bench diagnostics. */
bool tca8418_hal_debug_read_reg(uint8_t reg, uint8_t *value);
bool tca8418_hal_debug_write_reg(uint8_t reg, uint8_t value);

#endif
