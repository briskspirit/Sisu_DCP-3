#include "hal/tca8418_hal.h"

#include <string.h>

#include "hardware/gpio.h"
#include "hardware/i2c.h"
#include "hal/board.h"
#include "hal/keypad.h"
#include "services/log.h"
#include "services/timebase.h"
#include "pico/stdlib.h"

#define TCA_REG_CFG 0x01u
#define TCA_REG_INT_STAT 0x02u
#define TCA_REG_KEY_LCK_EC 0x03u
#define TCA_REG_KEY_EVENT_A 0x04u
#define TCA_REG_GPIO_INT_STAT1 0x11u /* ..3 at 0x11..0x13; read to clear latched GPI status */
#define TCA_REG_GPIO_DAT_STAT1 0x14u
#define TCA_REG_GPIO_DAT_STAT2 0x15u
#define TCA_REG_GPIO_DAT_OUT2 0x18u
#define TCA_REG_GPIO_INT_EN1 0x1au
#define TCA_REG_GPIO_INT_EN2 0x1bu
#define TCA_REG_GPIO_INT_EN3 0x1cu
#define TCA_REG_KP_GPIO1 0x1du
#define TCA_REG_KP_GPIO2 0x1eu
#define TCA_REG_KP_GPIO3 0x1fu
#define TCA_REG_GPIO_DIR1 0x23u
#define TCA_REG_GPIO_DIR2 0x24u
#define TCA_REG_GPIO_DIR3 0x25u
#define TCA_REG_GPIO_INT_LVL1 0x26u
#define TCA_REG_GPIO_INT_LVL2 0x27u
#define TCA_REG_GPIO_INT_LVL3 0x28u
#define TCA_REG_GPIO_PULL1 0x2cu
#define TCA_REG_GPIO_PULL2 0x2du

#define TCA_CFG_AUTO_INC 0x80u
#define TCA_CFG_KEY_INT_EN 0x01u
#define TCA_CFG_GPI_INT_EN 0x02u
#define TCA_INT_KEY 0x01u
#define TCA_INT_GPI 0x02u
#define TCA_INT_ANY 0x1fu

#define TCA_KEY_EVENT_PRESS 0x80u
#define TCA_KEY_EVENT_CODE_MASK 0x7fu
#define TCA_EVENT_COUNT_MASK 0x0fu

/* After this many consecutive timed-out/failed I2C transfers the bus is treated
 * as wedged: further transfers short-circuit (return false without touching the
 * bus) so a stuck SDA cannot stall the 8 ms main loop on every scan. Cleared by
 * tca8418_hal_init(), which a future bus-recovery re-init can call. */
#define TCA_I2C_FAIL_LIMIT 8u

/* When the TCA is not ready (failed init / latched-off bus), attempt a re-init
 * at most this often so a transient I2C fault self-heals without hammering. */
#define TCA_REINIT_INTERVAL_MS 1000u

/* The TCA already raises SYS_INT (GP42, active-low, shared with RTC/LTC) on key
 * events, so the FIFO is read only when INT is asserted; this is the backstop
 * cadence for an otherwise-idle line (missed / shared / level-stuck INT). */
#define TCA_SAFETY_POLL_MS 256u

/* Upper bound on FIFO drain re-read rounds per scan (FIFO depth is ~10; each
 * round pops up to 15). Guards against a misbehaving device that reports a
 * nonzero event count forever. */
#define TCA_DRAIN_MAX_ROUNDS 4u

#define TCA_COL3_BIT (1u << 3)
#define TCA_COL4_BIT (1u << 4)
#define TCA_COL5_BIT (1u << 5)
#define TCA_COL6_BIT (1u << 6)
#define TCA_ROW7_BIT (1u << 7)
#define TCA_CHARGER_STATUS_BITS (TCA_COL3_BIT | TCA_COL4_BIT)

/* Rows 2..5 are the 12 digit keys; row 0/1 carry the four function keys.
 * Top-key geometry follows the KiCad footprint matrix and can be corrected in
 * this one table if bench bring-up reveals a swapped dome. */
#define TCA_KEY_ROWS 6u
#define TCA_KEY_COLS 3u

static bool write_reg(uint8_t reg, uint8_t value);
static bool read_reg(uint8_t reg, uint8_t *value);
static bool read_regs(uint8_t reg, uint8_t *dst, uint8_t len);
static bool update_reg_bits(uint8_t reg, uint8_t mask, uint8_t value);
static bool configure_output_bit(uint8_t bit, bool high);
static bool read_output_bit(uint8_t bit, bool *high, bool *configured);
static bool drain_fifo_discard(void);
static bool drain_fifo_apply(uint8_t *event_count, bool *more_work);
static bool read_gpi_int_status(uint8_t status[3]);
static bool flush_gpi_int_status(void);
static void service_interrupt(bool force_fifo_poll,
                              tca8418_irq_result_t *out);
static uint16_t key_state_for_scan(void);
static uint16_t key_for_row_col(uint8_t row, uint8_t col);
static bool gpio_reg_bit_for_input(tca8418_input_t input, uint8_t *reg, uint8_t *mask);
static void note_i2c_result(bool ok);

static bool s_ready;
static bool s_bus_failed;
static uint8_t s_i2c_fail_count;
static uint16_t s_key_state;
/* The TCA FIFO can already contain PRESS+RELEASE when a keypad edge wakes the
 * RP from dormant. Applying both records directly to s_key_state collapses a
 * legitimate quick tap to zero before keypad_scan_raw() gets one sample. Keep
 * every observed press visible for exactly one scan; keypad_feed() then emits
 * the normal DOWN and its existing release debounce emits the later UP. */
static uint16_t s_key_press_latch;
static uint32_t s_next_reinit_ms;
static uint32_t s_next_safety_poll_ms;
/* Stable/raw HEAD_INT level most recently observed. Preserve it across a TCA
 * re-init so the normal-mode interrupt is armed for the opposite transition. */
static bool s_headset_level_high;
/* A charger GPI event may be drained by either shared_irq_service or the
 * keypad safety poll. Keep the observation here until battery_hal consumes it. */
static bool s_charger_status_change_pending;
/* Static zero initialization is intentionally fail-disabled for active-low
 * /CE. On the first init, adopt a still-configured expander's retained latch so
 * an RP-only reboot does not pulse /CE and restart the BQ safety timer. Once the
 * charger arbiter makes a request, retain that target across a TCA-only
 * recovery so re-init cannot briefly undo a supervisor inhibit. */
static bool s_charger_enabled_target;
static bool s_charger_target_explicit;

static bool read_headset_level(bool *level_high) {
    if (level_high == NULL) {
        return false;
    }
    uint8_t discard = 0u;
    uint8_t current = 0u;
    /* TI SCPS215G 8.6.2.8 requires GPIO_DAT_STAT to be read twice. Use the
     * second sample as the current debounced input level. */
    if (!read_reg(TCA_REG_GPIO_DAT_STAT1, &discard) ||
        !read_reg(TCA_REG_GPIO_DAT_STAT1, &current)) {
        return false;
    }
    *level_high = (current & TCA_ROW7_BIT) != 0u;
    s_headset_level_high = *level_high;
    return true;
}

static bool arm_headset_opposite_level(bool current_high) {
    /* HEAD_INT is LOW when removed and HIGH when inserted. Level/edge select
     * therefore alternates: wait for HIGH after removal, LOW after insertion.
     * If the contact moves during this write, level detection immediately
     * asserts for the new state instead of losing the transition. */
    s_headset_level_high = current_high;
    return write_reg(TCA_REG_GPIO_INT_LVL1,
                     current_high ? 0u : TCA_ROW7_BIT);
}

static bool read_charger_level_bits(uint8_t *level_bits) {
    if (level_bits == NULL) {
        return false;
    }
    uint8_t discard = 0u;
    uint8_t current = 0u;
    /* TI SCPS215G 8.6.2.8 requires GPIO_DAT_STAT to be read twice. STAT1 and
     * STAT2 occupy one byte, so the accepted sample is also an atomic pair. */
    if (!read_reg(TCA_REG_GPIO_DAT_STAT2, &discard) ||
        !read_reg(TCA_REG_GPIO_DAT_STAT2, &current)) {
        return false;
    }
    *level_bits = (uint8_t)(current & TCA_CHARGER_STATUS_BITS);
    return true;
}

static bool arm_charger_opposite_levels(uint8_t current_bits) {
    /* GPIO_INT_LVL selects one polarity per pin. Arm LOW after a HIGH sample
     * and HIGH after a LOW sample, so insertion, removal, completion, and fault
     * transitions all remain observable. This register is wholly HAL-owned. */
    return write_reg(
        TCA_REG_GPIO_INT_LVL2,
        (uint8_t)((~current_bits) & TCA_CHARGER_STATUS_BITS));
}

bool tca8418_hal_init(void) {
    s_ready = false;
    s_key_state = 0u;
    s_key_press_latch = 0u;
    s_bus_failed = false;
    s_i2c_fail_count = 0u;

    if (!s_charger_target_explicit) {
        bool retained_high = false;
        bool retained_configured = false;
        if (read_output_bit(TCA_COL5_BIT, &retained_high,
                            &retained_configured) &&
            retained_configured) {
            s_charger_enabled_target = !retained_high;
        } else {
            s_charger_enabled_target = false;
        }
    }
    uint8_t charger_output = s_charger_enabled_target
        ? 0u : TCA_COL5_BIT;

    /* Keyboard pins: ROW0..ROW5 and COL0..COL2. Everything else remains GPIO,
     * with COL5/COL6 as controlled outputs and COL7 left Hi-Z for the
     * legacy charger-control net until we intentionally implement that beacon. */
    if (!write_reg(TCA_REG_KP_GPIO1, 0x3fu) ||
        !write_reg(TCA_REG_KP_GPIO2, 0x07u) ||
        !write_reg(TCA_REG_KP_GPIO3, 0x00u) ||
        /* Safe output acquisition order: preload /CE from the durable software
         * target (fail-disabled before the arbiter starts), leave NiMH select
         * low, disconnect the POR pull-ups, then take output ownership. */
        !write_reg(TCA_REG_GPIO_DAT_OUT2, charger_output) ||
        !write_reg(TCA_REG_GPIO_PULL2, (uint8_t)(TCA_COL5_BIT | TCA_COL6_BIT)) ||
        /* ROW6 is unused and ROW7 is HEAD_INT; both are inputs. Explicit writes
         * unwind any stale expander ownership across a warm RP-only reboot. */
        !write_reg(TCA_REG_GPIO_DIR1, 0x00u) ||
        !write_reg(TCA_REG_GPIO_DIR2, (uint8_t)(TCA_COL5_BIT | TCA_COL6_BIT)) ||
        !write_reg(TCA_REG_GPIO_DIR3, 0x00u) ||
        /* The TCA8418 has internal PULL-UPS only (no pull-downs), enabled at POR;
         * a 1 in GPIO_PULL disables. Disable them on every detect input that has
         * its own external network (per the schematic, user-confirmed):
         *  - ROW7 HEAD_INT: external 470k pull-up to 3V3 vs the J2 NC contact
         *    to X_EAR_P/R62-100k-to-GND (raw 0 = empty ~0.58 V, raw 1 =
         *    inserted; rework 2026-07-10). The internal pull-up would parallel
         *    the 470k (~82k effective) and lift the empty level to ~1.8 V,
         *    breaking the divider -- it MUST stay disabled.
         *  - COL3/COL4 CHR_STAT1/2: internal pull-ups ENABLED. On Rev B2 these
         *    pins connect directly to the BQ25171 open-drain outputs; R59/R60
         *    pull them to CHR_AUX_3V3 only while charger input powers U17.
         *    The internal pulls therefore define detached as (1,1) while the
         *    external rail is absent, and a powered BQ can still sink either
         *    line. Board-1 bench confirmed detached (1,1), active (1,0), and
         *    clean live removal; dormant STAT2-low wake remains gated.
         *  - COL5 (charger /CE) / COL6 (TMUX1574 charger-config select): driven
         *    outputs. Rev B2 fits the COL6 100k pull-down, but its COL5 R77
         *    pull-down is DNP. The BQ25171's weak internal pull-down therefore
         *    loses to the TCA's POR pull-up until this initialization completes.
         *    Disable both TCA pull-ups and drive the safe NiMH mode plus the
         *    retained /CE target explicitly. R77 remains a required board
         *    rework for charger recovery while the RP/TCA rail is absent or
         *    below regulation.
         * Matrix pins (ROW0-5, COL0-2) keep their pull-ups: the keyscan engine
         * relies on them. */
        !write_reg(TCA_REG_GPIO_PULL1, (uint8_t)(1u << 7)) ||
        /* Disarm the powered-off charger wake configuration before flushing
         * stale status and restoring the normal keypad + headset sources. */
        !write_reg(TCA_REG_GPIO_INT_EN1, 0x00u) ||
        !write_reg(TCA_REG_GPIO_INT_EN2, 0x00u) ||
        !write_reg(TCA_REG_GPIO_INT_EN3, 0x00u)) {
        return false;
    }
    /* Discard-drain the key FIFO: the matrix scanner runs autonomously, so keys
     * pressed while the phone was off (or during a bus outage) are queued here
     * and would replay as phantom input through the first key_state() scan --
     * and INT_STAT.K_INT cannot even be cleared while unread events remain
     * (datasheet 8.6.2.2). Then flush the latched GPI status (a wake cause;
     * clears on read) so /INT genuinely releases before the INT_STAT clear. */
    uint8_t charger_level_bits = 0u;
    if (!drain_fifo_discard() ||
        !flush_gpi_int_status() ||
        !write_reg(TCA_REG_INT_STAT, TCA_INT_ANY) ||
        !read_charger_level_bits(&charger_level_bits) ||
        !arm_headset_opposite_level(s_headset_level_high) ||
        !arm_charger_opposite_levels(charger_level_bits) ||
        !write_reg(TCA_REG_GPIO_INT_EN1, TCA_ROW7_BIT) ||
        !write_reg(TCA_REG_GPIO_INT_EN2, TCA_CHARGER_STATUS_BITS) ||
        !write_reg(TCA_REG_CFG,
                   (uint8_t)(TCA_CFG_AUTO_INC | TCA_CFG_KEY_INT_EN |
                             TCA_CFG_GPI_INT_EN))) {
        return false;
    }

    /* Verify the config actually took. On a battery-insert cold boot the 3V3
     * rail ramps for seconds (boost soft-start); the TCA can ACK mid-ramp while
     * its core misbehaves -- bench 2026-07-05: ~7 s of all-zero GPIO reads
     * (phantom headset insert, STAT (0,0) "fault") right after such a boot.
     * A failed readback leaves s_ready false, and the 1 s re-init cadence
     * retries until the rail is real and the config sticks. */
    uint8_t kp1 = 0u;
    uint8_t cfg = 0u;
    uint8_t int_en1 = 0u;
    uint8_t int_en2 = 0u;
    uint8_t out2 = 0u;
    uint8_t dir2 = 0u;
    uint8_t pull2 = 0u;
    if (!read_reg(TCA_REG_KP_GPIO1, &kp1) || kp1 != 0x3fu ||
        !read_reg(TCA_REG_GPIO_DAT_OUT2, &out2) ||
        (out2 & (uint8_t)(TCA_COL5_BIT | TCA_COL6_BIT)) !=
            charger_output ||
        !read_reg(TCA_REG_GPIO_DIR2, &dir2) ||
        (dir2 & (uint8_t)(TCA_COL5_BIT | TCA_COL6_BIT)) !=
            (uint8_t)(TCA_COL5_BIT | TCA_COL6_BIT) ||
        !read_reg(TCA_REG_GPIO_PULL2, &pull2) ||
        (pull2 & (uint8_t)(TCA_COL5_BIT | TCA_COL6_BIT)) !=
            (uint8_t)(TCA_COL5_BIT | TCA_COL6_BIT) ||
        !read_reg(TCA_REG_GPIO_INT_EN1, &int_en1) ||
        (int_en1 & TCA_ROW7_BIT) == 0u ||
        !read_reg(TCA_REG_GPIO_INT_EN2, &int_en2) ||
        (int_en2 & TCA_CHARGER_STATUS_BITS) != TCA_CHARGER_STATUS_BITS ||
        !read_reg(TCA_REG_CFG, &cfg) ||
        (cfg & (uint8_t)(TCA_CFG_AUTO_INC | TCA_CFG_KEY_INT_EN |
                         TCA_CFG_GPI_INT_EN)) !=
            (uint8_t)(TCA_CFG_AUTO_INC | TCA_CFG_KEY_INT_EN |
                      TCA_CFG_GPI_INT_EN)) {
        return false;
    }

    s_ready = true;
    return true;
}

bool tca8418_hal_debug_read_reg(uint8_t reg, uint8_t *value) {
    return read_reg(reg, value);
}

bool tca8418_hal_debug_write_reg(uint8_t reg, uint8_t value) {
    return write_reg(reg, value);
}

static uint16_t key_state_for_scan(void) {
    uint16_t state = (uint16_t)(s_key_state | s_key_press_latch);
    s_key_press_latch = 0u;
    return state;
}

uint16_t tca8418_hal_key_state(void) {
    uint32_t now = time_ms();

    if (!s_ready) {
        /* Recovery: retry init at a bounded rate so a transient I2C fault
         * (or a failed boot init) self-heals. A failed attempt re-latches the bus
         * so steady-state callers keep cheaply short-circuiting between tries. */
        if (time_diff_ms(now, s_next_reinit_ms) >= 0) {
            s_next_reinit_ms = now + TCA_REINIT_INTERVAL_MS;
            if (tca8418_hal_init()) {
                LOGI("keypad", "TCA8418 I2C recovered");
            } else {
                s_bus_failed = true;
            }
        }
        if (!s_ready) {
            return key_state_for_scan();
        }
    } else {
        /* Keep the retry deadline pinned to "now" while healthy so the FIRST
         * retry after a future bus wedge fires immediately. s_next_reinit_ms is
         * otherwise written only inside the recovery branch above, so a wedge
         * that first occurs after ~24.85 days of uptime would evaluate the gate
         * against a stale value (0 from boot) -> time_diff negative -> the
         * keypad/charger/HEAD_INT reads stay dead until the clock wraps. */
        s_next_reinit_ms = now;
    }

    /* The core-0 shared-IRQ service normally drains the FIFO before this scan.
     * Keep a direct line check and periodic forced FIFO poll as a recovery
     * backstop for early boot, a missed edge, or a temporarily disabled
     * arbiter. */
    bool safety_due = time_diff_ms(now, s_next_safety_poll_ms) >= 0;
    if (!safety_due && gpio_get(SYS_INT_PIN) != 0) {
        return key_state_for_scan();
    }
    tca8418_irq_result_t result;
    service_interrupt(safety_due, &result);
    if (safety_due) {
        s_next_safety_poll_ms = now + TCA_SAFETY_POLL_MS;
    }
    return key_state_for_scan();
}

void tca8418_hal_service_interrupt(tca8418_irq_result_t *out) {
    service_interrupt(false, out);
}

bool tca8418_hal_input_level(tca8418_input_t input, bool *level) {
    if (level == 0) {
        return false;
    }
    /* Gate on verified init: during the battery-insert rail ramp the TCA can
     * ACK reads while returning garbage (see the init readback comment). Every
     * consumer treats a failed read as "keep the safe previous/default state",
     * so refusing here is strictly better than serving phantom levels. */
    if (!s_ready) {
        return false;
    }

    uint8_t reg = 0u;
    uint8_t mask = 0u;
    if (!gpio_reg_bit_for_input(input, &reg, &mask)) {
        return false;
    }

    if (input == TCA8418_PIN_ROW7_HEAD_INT) {
        if (!read_headset_level(level)) {
            return false;
        }
    } else {
        uint8_t value = 0u;
        if (!read_reg(reg, &value)) {
            return false;
        }
        *level = (value & mask) != 0u;
    }
    return true;
}

bool tca8418_hal_charger_status(bool *stat1_high, bool *stat2_high) {
    if (stat1_high == NULL || stat2_high == NULL || !s_ready) {
        return false;
    }

    uint8_t value = 0u;
    if (!read_charger_level_bits(&value)) {
        return false;
    }
    *stat1_high = (value & TCA_COL3_BIT) != 0u;
    *stat2_high = (value & TCA_COL4_BIT) != 0u;
    return true;
}

bool tca8418_hal_take_charger_status_change(void) {
    bool pending = s_charger_status_change_pending;
    s_charger_status_change_pending = false;
    return pending;
}

bool tca8418_hal_arm_charger_wake_interrupt(void) {
    if (!s_ready) {
        return false;
    }

    /* Trigger levels first, then per-pin enables, then a full drain+flush, and
     * the CFG interrupt-source swap last: a genuine insertion landing anywhere
     * after the flush stays latched in GPIO_INT_STAT and asserts /INT the moment
     * GPI_INT_EN goes live. If that happens before POWMAN's falling-edge slot is
     * armed, power_sleep_enter() catches the already-low line in its post-arm
     * level recheck instead. */
    if (!write_reg(TCA_REG_GPIO_INT_LVL1, 0x00u) ||
        !write_reg(TCA_REG_GPIO_INT_LVL2, 0x00u) ||        /* CHR_STAT2: interrupt on low */
        !write_reg(TCA_REG_GPIO_INT_LVL3, 0x00u) ||
        !write_reg(TCA_REG_GPIO_INT_EN1, 0x00u) ||
        !write_reg(TCA_REG_GPIO_INT_EN2, TCA_COL4_BIT) ||
        !write_reg(TCA_REG_GPIO_INT_EN3, 0x00u) ||
        !drain_fifo_discard() ||
        !flush_gpi_int_status() ||
        !write_reg(TCA_REG_INT_STAT, TCA_INT_ANY) ||
        !write_reg(TCA_REG_CFG, (uint8_t)(TCA_CFG_AUTO_INC | TCA_CFG_GPI_INT_EN))) {
        return false;
    }
    return true;
}

/* Discard-drain the key-event FIFO (bounded like the scan path). Events are
 * intentionally NOT applied to s_key_state: both callers run outside normal
 * scanning (boot/disarm, pre-sleep) where replaying stale presses is the bug. */
static bool drain_fifo_discard(void) {
    for (uint8_t round = 0u; round < TCA_DRAIN_MAX_ROUNDS; round++) {
        uint8_t count_reg = 0u;
        if (!read_reg(TCA_REG_KEY_LCK_EC, &count_reg)) {
            return false;
        }
        uint8_t count = count_reg & TCA_EVENT_COUNT_MASK;
        if (count == 0u) {
            return true;
        }
        while (count-- > 0u) {
            uint8_t event = 0u;
            if (!read_reg(TCA_REG_KEY_EVENT_A, &event) || event == 0u) {
                break;
            }
        }
    }
    uint8_t count_reg = 0u;
    return read_reg(TCA_REG_KEY_LCK_EC, &count_reg) &&
           (count_reg & TCA_EVENT_COUNT_MASK) == 0u;
}

static bool drain_fifo_apply(uint8_t *event_count, bool *more_work) {
    uint8_t applied = 0u;
    bool remaining = false;

    for (uint8_t round = 0u; round < TCA_DRAIN_MAX_ROUNDS; round++) {
        uint8_t count_reg = 0u;
        if (!read_reg(TCA_REG_KEY_LCK_EC, &count_reg)) {
            return false;
        }
        uint8_t count = count_reg & TCA_EVENT_COUNT_MASK;
        if (count == 0u) {
            if (event_count != NULL) {
                *event_count = applied;
            }
            if (more_work != NULL) {
                *more_work = false;
            }
            return true;
        }
        while (count-- > 0u) {
            uint8_t event = 0u;
            if (!read_reg(TCA_REG_KEY_EVENT_A, &event)) {
                return false;
            }
            if (event == 0u) {
                break;
            }
            if (applied != UINT8_MAX) {
                applied++;
            }

            uint8_t code = event & TCA_KEY_EVENT_CODE_MASK;
            if (code == 0u || code > 80u) {
                continue;
            }
            uint8_t index = (uint8_t)(code - 1u);
            uint8_t row = (uint8_t)(index / 10u);
            uint8_t col = (uint8_t)(index % 10u);
            uint16_t key = key_for_row_col(row, col);
            if (key == 0u) {
                continue;
            }
            if ((event & TCA_KEY_EVENT_PRESS) != 0u) {
                s_key_state |= key;
                s_key_press_latch |= key;
            } else {
                s_key_state &= (uint16_t)~key;
            }
        }
    }

    uint8_t count_reg = 0u;
    if (!read_reg(TCA_REG_KEY_LCK_EC, &count_reg)) {
        return false;
    }
    remaining = (count_reg & TCA_EVENT_COUNT_MASK) != 0u;
    if (event_count != NULL) {
        *event_count = applied;
    }
    if (more_work != NULL) {
        *more_work = remaining;
    }
    return true;
}

static bool read_gpi_int_status(uint8_t status[3]) {
    for (uint8_t i = 0u; i < 3u; i++) {
        if (!read_reg((uint8_t)(TCA_REG_GPIO_INT_STAT1 + i),
                      &status[i])) {
            return false;
        }
    }
    return true;
}

/* GPIO_INT_STAT1..3 clear on read. Read per-register (not a burst) so the
 * flush works regardless of the CFG auto-increment state at call time. */
static bool flush_gpi_int_status(void) {
    uint8_t status[3];
    return read_gpi_int_status(status);
}

static void service_interrupt(bool force_fifo_poll,
                              tca8418_irq_result_t *out) {
    tca8418_irq_result_t result;
    memset(&result, 0, sizeof(result));
    if (!s_ready) {
        if (out != NULL) {
            *out = result;
        }
        return;
    }

    uint8_t int_status = 0u;
    if (!read_reg(TCA_REG_INT_STAT, &int_status)) {
        if (out != NULL) {
            *out = result;
        }
        return;
    }
    result.int_status = (uint8_t)(int_status & TCA_INT_ANY);

    if ((result.int_status & TCA_INT_KEY) != 0u ||
        force_fifo_poll) {
        if (!drain_fifo_apply(&result.key_events,
                              &result.more_work)) {
            if (out != NULL) {
                *out = result;
            }
            return;
        }
        result.did_work = result.key_events != 0u;
    }
    if ((result.int_status & TCA_INT_GPI) != 0u) {
        if (!read_gpi_int_status(result.gpi_status)) {
            if (out != NULL) {
                *out = result;
            }
            return;
        }
        if ((result.gpi_status[0] & TCA_ROW7_BIT) != 0u) {
            bool current_high = false;
            /* Flip the trigger before clearing INT_STAT. Otherwise the TCA's
             * level semantics reassert /INT for the state that just woke us,
             * and the shared-line drain reports a false stuck source. */
            if (!read_headset_level(&current_high) ||
                !arm_headset_opposite_level(current_high)) {
                if (out != NULL) {
                    *out = result;
                }
                return;
            }
        }
        if ((result.gpi_status[1] & TCA_CHARGER_STATUS_BITS) != 0u) {
            uint8_t current_bits = 0u;
            /* Preserve the clear-on-read evidence before any fallible I2C
             * operation. The battery poll will then force an immediate VIN
             * qualification even if re-arming this interrupt needs recovery. */
            s_charger_status_change_pending = true;
            if (!read_charger_level_bits(&current_bits) ||
                !arm_charger_opposite_levels(current_bits)) {
                if (out != NULL) {
                    *out = result;
                }
                return;
            }
        }
        result.did_work = true;
    }
    if (result.int_status != 0u) {
        result.did_work = true;
        if (!write_reg(TCA_REG_INT_STAT, result.int_status)) {
            if (out != NULL) {
                *out = result;
            }
            return;
        }
    }

    uint8_t pending_status = 0u;
    if (!read_reg(TCA_REG_INT_STAT, &pending_status)) {
        if (out != NULL) {
            *out = result;
        }
        return;
    }
    result.more_work =
        result.more_work ||
        (pending_status & TCA_INT_ANY) != 0u;
    result.transport_ok = true;
    if (out != NULL) {
        *out = result;
    }
}

bool tca8418_hal_set_charger_enabled(bool enabled) {
    /* BQ25171 /CE is active low. */
    s_charger_enabled_target = enabled;
    s_charger_target_explicit = true;
    return configure_output_bit(TCA_COL5_BIT, !enabled);
}

bool tca8418_hal_get_charger_enabled(bool *enabled) {
    if (enabled == NULL || !s_ready) {
        return false;
    }
    bool high = false;
    bool configured = false;
    if (!read_output_bit(TCA_COL5_BIT, &high, &configured)) {
        return false;
    }
    if (!configured) {
        /* A responsive TCA with POR-like GPIO configuration has reset behind
         * our back. Force the normal key-scan recovery path to reinitialize it
         * with the arbiter's retained COL5 target. */
        s_ready = false;
        s_next_reinit_ms = time_ms();
        LOGW("keypad", "TCA8418 charger control lost; reinitializing");
        return false;
    }
    *enabled = !high;
    return true;
}

bool tca8418_hal_set_charger_li_ion_mode(bool enabled) {
    return configure_output_bit(TCA_COL6_BIT, enabled);
}

static bool configure_output_bit(uint8_t bit, bool high) {
    if (!s_ready || (bit != TCA_COL5_BIT && bit != TCA_COL6_BIT)) {
        return false;
    }

    /* Program the output latch before taking ownership. On a reset TCA this
     * prevents an accidental high pulse when direction changes to output. */
    if (!update_reg_bits(TCA_REG_GPIO_DAT_OUT2, bit, high ? bit : 0u) ||
        !update_reg_bits(TCA_REG_GPIO_PULL2, bit, bit) ||
        !update_reg_bits(TCA_REG_GPIO_DIR2, bit, bit)) {
        return false;
    }

    bool actual_high = false;
    bool configured = false;
    if (!read_output_bit(bit, &actual_high, &configured)) {
        return false;
    }
    if (!configured || actual_high != high) {
        s_ready = false;
        s_next_reinit_ms = time_ms();
        LOGW("keypad", "TCA8418 output control readback mismatch");
        return false;
    }
    return true;
}

static bool read_output_bit(uint8_t bit, bool *high, bool *configured) {
    if (high == NULL || configured == NULL) {
        return false;
    }
    uint8_t output = 0u;
    uint8_t direction = 0u;
    uint8_t pull = 0u;
    if (!read_reg(TCA_REG_GPIO_DAT_OUT2, &output) ||
        !read_reg(TCA_REG_GPIO_DIR2, &direction) ||
        !read_reg(TCA_REG_GPIO_PULL2, &pull)) {
        return false;
    }
    *high = (output & bit) != 0u;
    *configured = (direction & bit) != 0u && (pull & bit) != 0u;
    return true;
}

static void note_i2c_result(bool ok) {
    if (ok) {
        s_i2c_fail_count = 0u;
        return;
    }
    if (s_i2c_fail_count < 0xffu) {
        s_i2c_fail_count++;
    }
    if (s_i2c_fail_count >= TCA_I2C_FAIL_LIMIT && !s_bus_failed) {
        s_bus_failed = true;
        s_ready = false;
        s_key_state = 0u; /* release held keys so the UI doesn't see a stuck key during the outage */
        s_key_press_latch = 0u;
        LOGW("keypad", "TCA8418 I2C unresponsive; disabling until re-init");
    }
}

static bool write_reg(uint8_t reg, uint8_t value) {
    if (s_bus_failed) {
        return false;
    }
    uint8_t data[2] = {reg, value};
    bool ok = i2c_write_timeout_us(BOARD_I2C_PORT, TCA8418_I2C_ADDR, data, sizeof(data), false,
                                   BOARD_I2C_TIMEOUT_US) == (int)sizeof(data);
    note_i2c_result(ok);
    return ok;
}

static bool read_reg(uint8_t reg, uint8_t *value) {
    if (value == 0) {
        return false;
    }
    return read_regs(reg, value, 1u);
}

static bool read_regs(uint8_t reg, uint8_t *dst, uint8_t len) {
    if (dst == 0 || len == 0u) {
        return false;
    }
    if (s_bus_failed) {
        return false;
    }
    int written = i2c_write_timeout_us(BOARD_I2C_PORT, TCA8418_I2C_ADDR, &reg, 1u, true,
                                       BOARD_I2C_TIMEOUT_US);
    if (written != 1) {
        note_i2c_result(false);
        return false;
    }
    bool ok = i2c_read_timeout_us(BOARD_I2C_PORT, TCA8418_I2C_ADDR, dst, len, false,
                                  BOARD_I2C_TIMEOUT_US) == (int)len;
    note_i2c_result(ok);
    return ok;
}

static bool update_reg_bits(uint8_t reg, uint8_t mask, uint8_t value) {
    uint8_t current = 0u;
    if (!read_reg(reg, &current)) {
        return false;
    }
    current = (uint8_t)((current & (uint8_t)~mask) | (value & mask));
    return write_reg(reg, current);
}

static uint16_t key_for_row_col(uint8_t row, uint8_t col) {
    static const uint16_t KEYS[TCA_KEY_ROWS][TCA_KEY_COLS] = {
        {KEY_C,    KEY_NAVI, KEY_UP},
        {0u,       0u,     KEY_DOWN},
        {KEY_1,    KEY_2,  KEY_3},
        {KEY_4,    KEY_5,  KEY_6},
        {KEY_7,    KEY_8,  KEY_9},
        {KEY_STAR, KEY_0,  KEY_HASH},
    };
    if (row >= TCA_KEY_ROWS || col >= TCA_KEY_COLS) {
        return 0u;
    }
    return KEYS[row][col];
}

static bool gpio_reg_bit_for_input(tca8418_input_t input, uint8_t *reg, uint8_t *mask) {
    if (reg == 0 || mask == 0) {
        return false;
    }
    switch (input) {
    case TCA8418_PIN_ROW7_HEAD_INT:
        *reg = TCA_REG_GPIO_DAT_STAT1;
        *mask = (uint8_t)(1u << 7);
        return true;
    case TCA8418_PIN_COL3_CHR_STAT1:
        *reg = TCA_REG_GPIO_DAT_STAT2;
        *mask = TCA_COL3_BIT;
        return true;
    case TCA8418_PIN_COL4_CHR_STAT2:
        *reg = TCA_REG_GPIO_DAT_STAT2;
        *mask = TCA_COL4_BIT;
        return true;
    default:
        return false;
    }
}
