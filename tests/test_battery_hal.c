#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "hal/battery_hal.h"
#include "hal/ltc2959_hal.h"
#include "hardware/regs/powman.h"
#include "hardware/structs/powman.h"

static powman_hw_t s_powman;
powman_hw_t *powman_hw = &s_powman;
static ltc2959_snapshot_t s_ltc;
static bool s_power_reset;
static bool s_init_available = true;
static bool s_configure_available = true;
static uint16_t s_adc;

void adc_init(void) {}
void adc_gpio_init(unsigned int gpio) { (void)gpio; }
void adc_select_input(unsigned int input) { (void)input; }
uint16_t adc_read(void) { return s_adc; }
void gpio_set_input_enabled(unsigned int gpio, bool enabled) {
    (void)gpio; (void)enabled;
}
int32_t time_diff_ms(uint32_t a, uint32_t b) { return (int32_t)(a - b); }
bool tca8418_hal_take_charger_status_change(void) { return false; }
bool tca8418_hal_charger_status(bool *stat1, bool *stat2) {
    *stat1 = true; *stat2 = true; return true;
}
bool ltc2959_hal_init(uint32_t now_ms) {
    (void)now_ms;
    if (!s_init_available) return false;
    if (s_power_reset) s_ltc.power_reset_count++;
    s_power_reset = false;
    s_ltc.present = true;
    s_ltc.configured = s_configure_available;
    return s_configure_available;
}
void ltc2959_hal_poll(uint32_t now_ms, uint32_t period_ms) {
    (void)now_ms; (void)period_ms;
}
void ltc2959_hal_get_snapshot(ltc2959_snapshot_t *out) { *out = s_ltc; }

static void sample(uint32_t now, uint16_t mv) {
    s_ltc.sample_sequence++;
    s_ltc.conversion_sequence++;
    s_ltc.voltage_mv = mv;
    s_ltc.sample_valid = s_ltc.last_conversion_sample_valid = true;
    s_ltc.current_polarity_verified = s_ltc.continuity_valid = true;
    battery_hal_poll(now);
}

static void qualify(uint16_t mv) {
    for (uint32_t i = 1u; i <= 5u; i++) sample(i * 250u, mv);
}

static void reboot(uint32_t cause, uint32_t retained, bool gauge_reset) {
    s_powman.chip_reset = cause;
    s_powman.scratch[6] = retained;
    memset(&s_ltc, 0, sizeof(s_ltc));
    s_power_reset = gauge_reset;
    s_adc = 0u;
    battery_hal_init();
}

int main(void) {
    reboot(POWMAN_CHIP_RESET_HAD_POR_BITS, 0u, true);
    assert(battery_hal_chemistry() == BATTERY_CHEMISTRY_UNKNOWN);
    assert(!battery_hal_valid());
    qualify(3900u);
    assert(battery_hal_chemistry() == BATTERY_CHEMISTRY_LI_ION_1S);
    assert(battery_hal_valid());
    assert(battery_hal_gauge_profile()->power_on_mv == 3400u);
    assert(battery_hal_power_on_gate_from_mv(3399u, false) == BATTERY_POWER_ON_REFUSE);
    assert(battery_hal_power_on_gate_from_mv(3400u, false) == BATTERY_POWER_ON_OK);
    uint32_t retained = s_powman.scratch[6];
    assert(retained != 0u);
    sample(2000u, 3200u);
    assert(battery_hal_chemistry() == BATTERY_CHEMISTRY_LI_ION_1S);
    assert(s_powman.scratch[6] == retained);

    /* A failed conversion retains the old sample sequence but must break LOW
     * qualification. The HAL feeds conversion attempts, not only successes. */
    sample(3000u, 3550u);
    sample(8000u, 3550u);
    assert(!battery_hal_standby_ready());
    s_ltc.conversion_sequence++;
    s_ltc.last_conversion_sample_valid = false;
    battery_hal_poll(9000u);
    assert(!battery_hal_valid());
    assert(battery_hal_standby_ready());
    sample(13000u, 3550u);
    sample(18000u, 3550u);
    assert(!battery_hal_low_notice());
    sample(23000u, 3550u);
    assert(battery_hal_low_notice());
    assert(battery_hal_standby_ready());

    /* Watchdog/deep-sleep reboot keeps the latch even with a low cell. */
    reboot(POWMAN_CHIP_RESET_HAD_WATCHDOG_RESET_SWCORE_BITS, retained, false);
    assert(battery_hal_chemistry() == BATTERY_CHEMISTRY_UNKNOWN);
    qualify(3200u);
    assert(battery_hal_chemistry() == BATTERY_CHEMISTRY_LI_ION_1S);
    assert(s_powman.scratch[6] == retained);

    /* An ACR-only reset changes accounting, not chemistry. */
    s_ltc.gauge_session++;
    sample(2000u, 2700u);
    assert(battery_hal_chemistry() == BATTERY_CHEMISTRY_LI_ION_1S);

    /* Lost RP retention with a still-powered gauge must not guess NiMH. */
    s_powman.scratch[6] = 0u;
    memset(&s_ltc, 0, sizeof(s_ltc));
    battery_hal_init();
    qualify(3000u);
    assert(battery_hal_chemistry() == BATTERY_CHEMISTRY_UNKNOWN);
    assert(!battery_hal_valid());
    assert(battery_hal_power_on_gate(false) == BATTERY_POWER_ON_PENDING);

    /* After the initial gauge acquisition, later reset evidence is not proof
     * of insertion, even if the RP's last reset-cause register still says POR. */
    s_power_reset = true;
    assert(ltc2959_hal_init(2000u));
    for (uint32_t i = 0u; i < 5u; i++) sample(2250u + i * 250u, 2700u);
    assert(battery_hal_chemistry() == BATTERY_CHEMISTRY_UNKNOWN);
    uint32_t uncertain = s_powman.scratch[6];
    assert(uncertain != 0u);

    /* An actual cold insertion still supports legacy NiMH under the contract. */
    reboot(POWMAN_CHIP_RESET_HAD_POR_BITS, 0u, true);
    qualify(2700u);
    assert(battery_hal_chemistry() == BATTERY_CHEMISTRY_NIMH_2S);
    assert(battery_hal_gauge_profile()->power_on_mv == 2100u);
    assert(s_powman.scratch[6] != retained);
    uint32_t retained_nimh = s_powman.scratch[6];

    /* Neither retained Li-ion nor an already-uncertain identity authorizes a
     * low-voltage NiMH fallback when the gauge subsequently loses power. */
    reboot(POWMAN_CHIP_RESET_HAD_WATCHDOG_RESET_SWCORE_BITS, retained, true);
    qualify(3100u);
    assert(battery_hal_chemistry() == BATTERY_CHEMISTRY_UNKNOWN);
    assert(!battery_hal_valid());
    assert(battery_hal_power_on_gate(false) == BATTERY_POWER_ON_PENDING);
    reboot(POWMAN_CHIP_RESET_HAD_WATCHDOG_RESET_SWCORE_BITS, uncertain, true);
    qualify(2700u);
    assert(battery_hal_chemistry() == BATTERY_CHEMISTRY_UNKNOWN);
    qualify(3900u);
    assert(battery_hal_chemistry() == BATTERY_CHEMISTRY_LI_ION_1S);

    const uint32_t rejected_causes[] = {
        0u, POWMAN_CHIP_RESET_HAD_BOR_BITS, POWMAN_CHIP_RESET_HAD_RUN_LOW_BITS,
        POWMAN_CHIP_RESET_HAD_WATCHDOG_RESET_SWCORE_BITS,
        POWMAN_CHIP_RESET_HAD_WATCHDOG_RESET_POWMAN_BITS,
        POWMAN_CHIP_RESET_HAD_SWCORE_PD_BITS,
        POWMAN_CHIP_RESET_HAD_POR_BITS | POWMAN_CHIP_RESET_HAD_BOR_BITS,
    };
    for (unsigned i = 0u; i < sizeof(rejected_causes) / sizeof(rejected_causes[0]); i++) {
        reboot(rejected_causes[i], 0u, true);
        qualify(2700u);
        assert(battery_hal_chemistry() == BATTERY_CHEMISTRY_UNKNOWN);
        assert(!battery_hal_valid());
    }
    reboot(POWMAN_CHIP_RESET_HAD_POR_BITS, retained ^ 1u, true);
    qualify(2700u);
    assert(battery_hal_chemistry() == BATTERY_CHEMISTRY_UNKNOWN);

    /* A transient I2C failure before the first read does not consume the one
     * cold-start opportunity; a successful read with no reset does. */
    s_init_available = false;
    reboot(POWMAN_CHIP_RESET_HAD_POR_BITS, 0u, true);
    s_init_available = true;
    assert(ltc2959_hal_init(0u));
    qualify(2700u);
    assert(battery_hal_chemistry() == BATTERY_CHEMISTRY_NIMH_2S);
    reboot(POWMAN_CHIP_RESET_HAD_POR_BITS, 0u, false);
    qualify(2700u);
    assert(battery_hal_chemistry() == BATTERY_CHEMISTRY_UNKNOWN);
    s_ltc.power_reset_count++;
    for (uint32_t i = 0u; i < 5u; i++) sample(2000u + i * 250u, 2700u);
    assert(battery_hal_chemistry() == BATTERY_CHEMISTRY_UNKNOWN);

    /* Charger voltage cannot identify a newly inserted battery. */
    reboot(POWMAN_CHIP_RESET_HAD_POR_BITS, 0u, true);
    s_adc = 4095u;
    battery_hal_init();
    qualify(4200u);
    assert(battery_hal_chemistry() == BATTERY_CHEMISTRY_UNKNOWN);
    assert(!battery_hal_valid());

    /* Retention is not boot-time authority until a fresh gauge acquisition
     * has ruled out a power reset. Keep the word during a transport outage. */
    s_init_available = false;
    reboot(POWMAN_CHIP_RESET_HAD_WATCHDOG_RESET_SWCORE_BITS, retained, false);
    assert(battery_hal_chemistry() == BATTERY_CHEMISTRY_UNKNOWN);
    assert(!battery_hal_valid());
    assert(s_powman.scratch[6] == retained);
    assert(battery_hal_power_on_gate_from_mv(3900u, false) == BATTERY_POWER_ON_PENDING);
    s_init_available = true;
    assert(ltc2959_hal_init(1000u));
    battery_hal_poll(1000u);
    assert(battery_hal_chemistry() == BATTERY_CHEMISTRY_UNKNOWN);
    sample(1250u, 2900u);
    assert(battery_hal_chemistry() == BATTERY_CHEMISTRY_LI_ION_1S);
    assert(s_powman.scratch[6] == retained);

    /* A runtime I2C gap is not new identity evidence and does not discard a
     * chemistry whose boot continuity has already been checked. */
    s_ltc.sample_valid = s_ltc.last_conversion_sample_valid = false;
    battery_hal_poll(1500u);
    assert(battery_hal_chemistry() == BATTERY_CHEMISTRY_LI_ION_1S);
    assert(!battery_hal_valid());
    sample(1750u, 2900u);
    assert(battery_hal_chemistry() == BATTERY_CHEMISTRY_LI_ION_1S);

    /* The same boot validation applies after dormant reset and to both
     * chemistries; a good gauge permits retention even at depleted voltage. */
    const uint32_t words[] = {retained_nimh, retained};
    const battery_chemistry_t chemistries[] = {
        BATTERY_CHEMISTRY_NIMH_2S, BATTERY_CHEMISTRY_LI_ION_1S,
    };
    for (unsigned i = 0u; i < 2u; i++) {
        reboot(POWMAN_CHIP_RESET_HAD_SWCORE_PD_BITS, words[i], false);
        assert(battery_hal_chemistry() == BATTERY_CHEMISTRY_UNKNOWN);
        s_ltc.conversion_sequence++;
        s_ltc.last_conversion_sample_valid = false;
        battery_hal_poll(250u);
        assert(battery_hal_chemistry() == BATTERY_CHEMISTRY_UNKNOWN);
        assert(s_powman.scratch[6] == words[i]);
        sample(500u, 2700u);
        assert(battery_hal_chemistry() == chemistries[i]);
        assert(s_powman.scratch[6] == words[i]);
    }

    /* A delayed first read that DOES report gauge reset invalidates retention
     * before any low-voltage samples can authorize charging. */
    s_init_available = false;
    reboot(POWMAN_CHIP_RESET_HAD_WATCHDOG_RESET_SWCORE_BITS, retained, true);
    assert(battery_hal_chemistry() == BATTERY_CHEMISTRY_UNKNOWN);
    s_init_available = true;
    assert(ltc2959_hal_init(1000u));
    qualify(2900u);
    assert(battery_hal_chemistry() == BATTERY_CHEMISTRY_UNKNOWN);
    assert(s_powman.scratch[6] != retained);

    /* A successful boot-register read without POR closes insertion permission
     * even if later configuration fails. A subsequent UVLO is not insertion. */
    s_configure_available = false;
    reboot(POWMAN_CHIP_RESET_HAD_POR_BITS, 0u, false);
    assert(s_ltc.present && !s_ltc.configured);
    s_configure_available = true;
    s_power_reset = true;
    assert(ltc2959_hal_init(1000u));
    qualify(2900u);
    assert(battery_hal_chemistry() == BATTERY_CHEMISTRY_UNKNOWN);

    /* Configuration failure must not erase actual initial POR evidence. */
    s_configure_available = false;
    reboot(POWMAN_CHIP_RESET_HAD_POR_BITS, 0u, true);
    s_configure_available = true;
    assert(ltc2959_hal_init(1000u));
    qualify(2700u);
    assert(battery_hal_chemistry() == BATTERY_CHEMISTRY_NIMH_2S);
    puts("Battery HAL chemistry integration tests passed");
    return 0;
}
