#include <assert.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "hal/tca8418_hal.h"
#include "hardware/i2c.h"
#include "services/log.h"

#define REG_DAT_OUT2 0x18u
#define REG_DIR2 0x24u
#define REG_PULL2 0x2du
#define COL5 (1u << 5)
#define COL6 (1u << 6)

static i2c_inst_t s_i2c;
i2c_inst_t *const i2c0 = &s_i2c;
uint64_t host_test_time_us;

static uint8_t s_regs[256];
static uint8_t s_selected_reg;
static uint32_t s_now_ms;
typedef struct {
    uint8_t reg;
    uint8_t value;
} write_event_t;
static write_event_t s_writes[64];
static size_t s_write_count;

int i2c_write_timeout_us(i2c_inst_t *i2c,
                         uint8_t addr,
                         const uint8_t *src,
                         size_t len,
                         bool nostop,
                         unsigned int timeout_us) {
    (void)i2c;
    (void)nostop;
    (void)timeout_us;
    if (addr != 0x34u || src == NULL || len == 0u) {
        return -1;
    }
    s_selected_reg = src[0];
    for (size_t i = 1u; i < len; i++) {
        uint8_t reg = (uint8_t)(s_selected_reg + i - 1u);
        s_regs[reg] = src[i];
        assert(s_write_count < sizeof(s_writes) / sizeof(s_writes[0]));
        s_writes[s_write_count++] = (write_event_t){reg, src[i]};
    }
    return (int)len;
}

int i2c_read_timeout_us(i2c_inst_t *i2c,
                        uint8_t addr,
                        uint8_t *dst,
                        size_t len,
                        bool nostop,
                        unsigned int timeout_us) {
    (void)i2c;
    (void)nostop;
    (void)timeout_us;
    if (addr != 0x34u || dst == NULL || len == 0u) {
        return -1;
    }
    memcpy(dst, &s_regs[s_selected_reg], len);
    return (int)len;
}

bool gpio_get(unsigned int gpio) {
    (void)gpio;
    return true;
}

uint32_t time_ms(void) {
    return s_now_ms;
}

int32_t time_diff_ms(uint32_t a, uint32_t b) {
    return (int32_t)(a - b);
}

void log_write(log_level_t level, const char *tag, const char *fmt, ...) {
    (void)level;
    (void)tag;
    (void)fmt;
}

static size_t first_write_to(uint8_t reg) {
    for (size_t i = 0u; i < s_write_count; i++) {
        if (s_writes[i].reg == reg) {
            return i;
        }
    }
    return SIZE_MAX;
}

static void test_warm_rp_boot_preserves_retained_charger_target(void) {
    memset(s_regs, 0, sizeof(s_regs));
    s_regs[REG_DIR2] = COL5 | COL6;
    s_regs[REG_PULL2] = COL5 | COL6;
    s_regs[REG_DAT_OUT2] = 0u;
    s_write_count = 0u;
    s_now_ms = 0u;

    assert(tca8418_hal_init());
    size_t latch_write = first_write_to(REG_DAT_OUT2);
    assert(latch_write != SIZE_MAX);
    assert((s_writes[latch_write].value & COL5) == 0u);
    assert((s_regs[REG_DAT_OUT2] & COL5) == 0u);

    bool enabled = false;
    assert(tca8418_hal_get_charger_enabled(&enabled));
    assert(enabled);

    s_regs[REG_DIR2] = COL5 | COL6;
    s_regs[REG_PULL2] = COL5 | COL6;
    s_regs[REG_DAT_OUT2] = COL5;
    s_write_count = 0u;

    assert(tca8418_hal_init());
    latch_write = first_write_to(REG_DAT_OUT2);
    assert(latch_write != SIZE_MAX);
    assert((s_writes[latch_write].value & COL5) != 0u);
    assert(tca8418_hal_get_charger_enabled(&enabled));
    assert(!enabled);
}

static void test_charger_control_is_verified_and_recovers(void) {
    /* A cold/reset expander has no retained output ownership. Until the arbiter
     * makes an explicit request, initialization must therefore fail-disabled. */
    memset(s_regs, 0, sizeof(s_regs));
    s_write_count = 0u;
    s_now_ms = 0u;
    assert(tca8418_hal_init());
    assert((s_regs[REG_DAT_OUT2] & (COL5 | COL6)) == COL5);
    assert((s_regs[REG_DIR2] & (COL5 | COL6)) == (COL5 | COL6));
    assert((s_regs[REG_PULL2] & (COL5 | COL6)) == (COL5 | COL6));
    size_t latch_write = first_write_to(REG_DAT_OUT2);
    size_t pull_write = first_write_to(REG_PULL2);
    size_t direction_write = first_write_to(REG_DIR2);
    assert(latch_write != SIZE_MAX && pull_write != SIZE_MAX &&
           direction_write != SIZE_MAX);
    assert(latch_write < pull_write && pull_write < direction_write);

    bool enabled = false;
    assert(tca8418_hal_get_charger_enabled(&enabled));
    assert(!enabled);

    assert(tca8418_hal_set_charger_enabled(true));
    assert(tca8418_hal_get_charger_enabled(&enabled));
    assert(enabled);

    s_write_count = 0u;
    assert(tca8418_hal_set_charger_enabled(false));
    latch_write = first_write_to(REG_DAT_OUT2);
    pull_write = first_write_to(REG_PULL2);
    direction_write = first_write_to(REG_DIR2);
    assert(latch_write < pull_write && pull_write < direction_write);
    assert(tca8418_hal_get_charger_enabled(&enabled));
    assert(!enabled);
    assert((s_regs[REG_DAT_OUT2] & COL5) != 0u);

    assert(tca8418_hal_set_charger_enabled(true));

    /* Emulate a TCA-only reset/misconfiguration. Readback must reject it, and
     * the next keypad scan must preserve the last explicit enabled target. */
    s_regs[REG_DIR2] &= (uint8_t)~COL5;
    s_regs[REG_PULL2] &= (uint8_t)~COL5;
    s_regs[REG_DAT_OUT2] |= COL5;
    s_now_ms = 100u;
    assert(!tca8418_hal_get_charger_enabled(&enabled));
    (void)tca8418_hal_key_state();
    assert(tca8418_hal_get_charger_enabled(&enabled));
    assert(enabled);
    assert((s_regs[REG_DAT_OUT2] & COL5) == 0u);
    assert((s_regs[REG_DIR2] & COL5) != 0u);
    assert((s_regs[REG_PULL2] & COL5) != 0u);

    /* The same recovery must preserve a supervisor's disabled target. */
    assert(tca8418_hal_set_charger_enabled(false));
    s_regs[REG_DIR2] &= (uint8_t)~COL5;
    s_regs[REG_PULL2] &= (uint8_t)~COL5;
    s_regs[REG_DAT_OUT2] &= (uint8_t)~COL5;
    s_now_ms++;
    assert(!tca8418_hal_get_charger_enabled(&enabled));
    (void)tca8418_hal_key_state();
    assert(tca8418_hal_get_charger_enabled(&enabled));
    assert(!enabled);
    assert((s_regs[REG_DAT_OUT2] & COL5) != 0u);
}

int main(void) {
    test_warm_rp_boot_preserves_retained_charger_target();
    test_charger_control_is_verified_and_recovers();
    puts("TCA8418 charger-control tests passed");
    return 0;
}
