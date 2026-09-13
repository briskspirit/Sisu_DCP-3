#include "services/modem_maintenance.h"

#include <limits.h>
#include <stdio.h>
#include <string.h>

#define MODEM_MAINTENANCE_RETRY_LIMIT 2u
#define MODEM_MAINTENANCE_RECORD_MAX 32u

typedef enum {
    MAINT_PHASE_NONE = 0,
    MAINT_SCAN_QUERY,
    MAINT_SCAN_SET,
    MAINT_SCAN_VERIFY,
    MAINT_BAND_MODE_QUERY,
    MAINT_BAND_BASE_QUERY,
    MAINT_BAND_SET_RAM_MODE,
    MAINT_BAND_VERIFY_RAM_MODE,
    MAINT_BAND_SET_PRESET,
    MAINT_BAND_VERIFY_PRESET,
    MAINT_BAND_RESTORE_SET_RAM_MODE,
    MAINT_BAND_RESTORE_VERIFY_RAM_MODE,
    MAINT_BAND_RESTORE_CONFIG,
    MAINT_BAND_RESTORE_VERIFY_CONFIG,
    MAINT_BAND_RESTORE_MODE,
    MAINT_BAND_RESTORE_VERIFY_MODE,
    MAINT_ANT_SET_CFUN4,
    MAINT_ANT_VERIFY_CFUN4,
    MAINT_ANT_VERIFY_TUNER_ENABLED,
    MAINT_ANT_VERIFY_TUNER_TABLE,
    MAINT_ANT_VERIFY_GPIO_A_ALT,
    MAINT_ANT_VERIFY_GPIO_B_ALT,
    MAINT_ANT_DISABLE_TUNER,
    MAINT_ANT_VERIFY_TUNER_DISABLED,
    MAINT_ANT_RELEASE_GPIO_A,
    MAINT_ANT_VERIFY_GPIO_A_INPUT,
    MAINT_ANT_RELEASE_GPIO_B,
    MAINT_ANT_VERIFY_GPIO_B_INPUT,
    MAINT_ANT_SET_GPIO_A,
    MAINT_ANT_VERIFY_GPIO_A,
    MAINT_ANT_SET_GPIO_B,
    MAINT_ANT_VERIFY_GPIO_B,
    MAINT_ANT_RESTORE_RELEASE_GPIO_A,
    MAINT_ANT_RESTORE_VERIFY_GPIO_A_INPUT,
    MAINT_ANT_RESTORE_RELEASE_GPIO_B,
    MAINT_ANT_RESTORE_VERIFY_GPIO_B_INPUT,
    MAINT_ANT_RESTORE_GPIO_A_ALT,
    MAINT_ANT_RESTORE_VERIFY_GPIO_A_ALT,
    MAINT_ANT_RESTORE_GPIO_B_ALT,
    MAINT_ANT_RESTORE_VERIFY_GPIO_B_ALT,
    MAINT_ANT_RESTORE_DISABLE_TUNER,
    MAINT_ANT_RESTORE_VERIFY_TUNER_DISABLED,
    MAINT_ANT_RESTORE_ROW_RF2,
    MAINT_ANT_RESTORE_ROW_RF3,
    MAINT_ANT_RESTORE_ROW_RF4,
    MAINT_ANT_RESTORE_ROW_RF1,
    MAINT_ANT_RESTORE_VERIFY_TABLE,
    MAINT_ANT_RESTORE_VERIFY_ENABLED,
    MAINT_ANT_RESTORE_SET_CFUN5,
    MAINT_ANT_RESTORE_VERIFY_CFUN5,
    MAINT_FAIL_CLOSED_SET_CFUN4,
    MAINT_FAIL_CLOSED_VERIFY_CFUN4,
} modem_maintenance_phase_t;

static const modem_maintenance_backend_t *s_backend;
static modem_maintenance_hooks_t s_hooks;
static modem_maintenance_snapshot_t s_maintenance;
static modem_maintenance_phase_t s_maintenance_phase;
static bool s_maintenance_line_seen;
static bool s_maintenance_line_invalid;
static bool s_maintenance_cancel_requested;
static bool s_maintenance_mutated;
static bool s_maintenance_finishes_error;
static bool s_maintenance_recovery_checked;
static modem_maintenance_error_t s_maintenance_original_error;
static uint8_t s_maintenance_original_band_mode;
static modem_band_config_t s_maintenance_original_band;
static modem_band_config_t s_maintenance_target_band;
static uint8_t s_maintenance_expected_u8;
static uint16_t s_maintenance_expected_u16;
static modem_band_config_t s_maintenance_expected_band;
static modem_gpio_config_t s_maintenance_expected_gpio;
static uint8_t s_maintenance_observed_u8;
static uint16_t s_maintenance_observed_u16;
static modem_band_config_t s_maintenance_observed_band;
static modem_gpio_config_t s_maintenance_observed_gpio;
static bool s_maintenance_observed_bool;

static bool maintenance_band_equal(const modem_band_config_t *a,
                                   const modem_band_config_t *b) {
    return a != NULL && b != NULL && a->gsm == b->gsm &&
           a->wcdma == b->wcdma && a->lte == b->lte;
}

static bool maintenance_phase_is_band_restore(
    modem_maintenance_phase_t phase) {
    return phase >= MAINT_BAND_RESTORE_SET_RAM_MODE &&
           phase <= MAINT_BAND_RESTORE_VERIFY_MODE;
}

static bool maintenance_phase_is_antenna_restore(
    modem_maintenance_phase_t phase) {
    return phase >= MAINT_ANT_RESTORE_RELEASE_GPIO_A &&
           phase <= MAINT_ANT_RESTORE_VERIFY_CFUN5;
}

static void maintenance_publish(modem_maintenance_state_t state,
                                modem_maintenance_error_t error) {
    s_maintenance.state = state;
    s_maintenance.error = error;
    s_maintenance.step = (uint8_t)s_maintenance_phase;
    s_maintenance.sequence++;
    if (s_maintenance.sequence == 0u) {
        s_maintenance.sequence = 1u;
    }
}

static void maintenance_set_phase(modem_maintenance_phase_t phase,
                                  modem_maintenance_state_t state) {
    s_maintenance_phase = phase;
    s_maintenance_line_seen = false;
    s_maintenance_line_invalid = false;
    s_maintenance.retries = 0u;
    s_maintenance.step = (uint8_t)phase;
    s_maintenance.state = state;
    s_maintenance.sequence++;
    if (s_maintenance.sequence == 0u) {
        s_maintenance.sequence = 1u;
    }
}

static void maintenance_finish_terminal(modem_maintenance_state_t state,
                                        modem_maintenance_error_t error) {
    s_maintenance_phase = MAINT_PHASE_NONE;
    s_maintenance_cancel_requested = false;
    s_maintenance_mutated = false;
    s_maintenance_finishes_error = false;
    s_maintenance_original_error = MODEM_MAINTENANCE_ERROR_NONE;
    s_maintenance.retries = 0u;
    maintenance_publish(state, error);
}

static bool maintenance_record_format(char *out, size_t out_cap) {
    if (out == NULL || out_cap == 0u) {
        return false;
    }
    int length = snprintf(out, out_cap, "B%u,%u,%u,%llX",
                          (unsigned)s_maintenance_original_band_mode,
                          (unsigned)s_maintenance_original_band.gsm,
                          (unsigned)s_maintenance_original_band.wcdma,
                          (unsigned long long)s_maintenance_original_band.lte);
    return length > 0 && (size_t)length < out_cap;
}

static bool maintenance_record_parse(const char *text, uint8_t *mode,
                                     modem_band_config_t *config) {
    unsigned parsed_mode = 0u;
    unsigned gsm = 0u;
    unsigned wcdma = 0u;
    unsigned long long lte = 0u;
    int consumed = 0;
    if (text == NULL || mode == NULL || config == NULL ||
        sscanf(text, "B%u,%u,%u,%llX%n", &parsed_mode, &gsm, &wcdma,
               &lte, &consumed) != 4 || text[consumed] != '\0' ||
        parsed_mode > 1u || gsm > UINT8_MAX || wcdma > UINT16_MAX ||
        lte == 0u) {
        return false;
    }
    *mode = (uint8_t)parsed_mode;
    config->gsm = (uint8_t)gsm;
    config->wcdma = (uint16_t)wcdma;
    config->lte = (uint64_t)lte;
    return true;
}

static bool maintenance_record_store(void) {
    char record[MODEM_MAINTENANCE_RECORD_MAX];
    if (!maintenance_record_format(record, sizeof(record)) ||
        s_hooks.write_recovery_record == NULL ||
        !s_hooks.write_recovery_record(record)) {
        return false;
    }
    /* RAM takes ownership immediately after the record changes. If durability
     * cannot be proven, mutation remains forbidden and restoration owns the
     * next action. */
    s_maintenance.recovery_pending = true;
    s_maintenance.sequence++;
    return s_hooks.flush_recovery_record != NULL &&
           s_hooks.flush_recovery_record();
}

static bool maintenance_record_clear(void) {
    if (s_hooks.write_recovery_record == NULL ||
        !s_hooks.write_recovery_record("") ||
        s_hooks.flush_recovery_record == NULL ||
        !s_hooks.flush_recovery_record()) {
        return false;
    }
    s_maintenance.recovery_pending = false;
    s_maintenance.sequence++;
    return true;
}

static void maintenance_begin_fail_closed(
    modem_maintenance_error_t error) {
    s_maintenance_original_error = error != MODEM_MAINTENANCE_ERROR_NONE
        ? error : MODEM_MAINTENANCE_ERROR_READBACK;
    s_maintenance_finishes_error = true;
    s_maintenance_mutated = true;
    maintenance_set_phase(MAINT_FAIL_CLOSED_SET_CFUN4,
                          MODEM_MAINTENANCE_RESTORING);
}

static void maintenance_begin_band_restore(
    bool finish_with_error, modem_maintenance_error_t error,
    bool command_in_flight) {
    if (command_in_flight) {
        s_maintenance_cancel_requested = true;
        if (finish_with_error) {
            s_maintenance_finishes_error = true;
            s_maintenance_original_error = error;
        }
        return;
    }
    if (maintenance_phase_is_band_restore(s_maintenance_phase)) {
        return;
    }
    s_maintenance.band_test_active = false;
    s_maintenance_cancel_requested = false;
    s_maintenance_finishes_error = finish_with_error;
    s_maintenance_original_error = finish_with_error
        ? error : MODEM_MAINTENANCE_ERROR_NONE;
    maintenance_set_phase(MAINT_BAND_RESTORE_SET_RAM_MODE,
                          MODEM_MAINTENANCE_RESTORING);
}

static void maintenance_begin_antenna_restore(
    bool finish_with_error, modem_maintenance_error_t error,
    bool command_in_flight) {
    if (command_in_flight) {
        s_maintenance_cancel_requested = true;
        if (finish_with_error) {
            s_maintenance_finishes_error = true;
            s_maintenance_original_error = error;
        }
        return;
    }
    if (maintenance_phase_is_antenna_restore(s_maintenance_phase)) {
        return;
    }
    s_maintenance.antenna_active = false;
    s_maintenance_cancel_requested = false;
    s_maintenance_finishes_error = finish_with_error;
    s_maintenance_original_error = finish_with_error
        ? error : MODEM_MAINTENANCE_ERROR_NONE;
    maintenance_set_phase(MAINT_ANT_RESTORE_RELEASE_GPIO_A,
                          MODEM_MAINTENANCE_RESTORING);
}

static void maintenance_begin(modem_maintenance_action_t action,
                              modem_maintenance_phase_t phase) {
    s_maintenance.action = action;
    s_maintenance.error = MODEM_MAINTENANCE_ERROR_NONE;
    s_maintenance.band_preset = 0u;
    s_maintenance.antenna_rf = 0u;
    s_maintenance.band_test_active = false;
    s_maintenance.antenna_active = false;
    s_maintenance_cancel_requested = false;
    s_maintenance_mutated = false;
    s_maintenance_finishes_error = false;
    s_maintenance_original_error = MODEM_MAINTENANCE_ERROR_NONE;
    maintenance_set_phase(phase, MODEM_MAINTENANCE_PENDING);
}

void modem_maintenance_init(const modem_maintenance_backend_t *backend,
                            const modem_maintenance_hooks_t *hooks) {
    s_backend = backend;
    memset(&s_hooks, 0, sizeof(s_hooks));
    if (hooks != NULL) {
        s_hooks = *hooks;
    }
    modem_maintenance_reset_session();
}

void modem_maintenance_reset_session(void) {
    memset(&s_maintenance, 0, sizeof(s_maintenance));
    s_maintenance.sequence = 1u;
    s_maintenance.state = MODEM_MAINTENANCE_IDLE;
    s_maintenance_phase = MAINT_PHASE_NONE;
    s_maintenance_line_seen = false;
    s_maintenance_line_invalid = false;
    s_maintenance_cancel_requested = false;
    s_maintenance_mutated = false;
    s_maintenance_finishes_error = false;
    s_maintenance_recovery_checked = false;
    s_maintenance_original_error = MODEM_MAINTENANCE_ERROR_NONE;
    memset(&s_maintenance_original_band, 0,
           sizeof(s_maintenance_original_band));
    memset(&s_maintenance_target_band, 0,
           sizeof(s_maintenance_target_band));
}

bool modem_maintenance_supported(void) {
    return s_backend != NULL && s_backend->supported;
}

bool modem_maintenance_can_admit(void) {
    return modem_maintenance_supported() && !s_maintenance.recovery_pending &&
           (s_maintenance.state == MODEM_MAINTENANCE_IDLE ||
            s_maintenance.state == MODEM_MAINTENANCE_DONE ||
            s_maintenance.state == MODEM_MAINTENANCE_ERROR);
}

bool modem_maintenance_request_scan_timer(uint16_t seconds) {
    if (seconds < 5u || seconds > 3600u ||
        !modem_maintenance_can_admit()) {
        return false;
    }
    maintenance_begin(MODEM_MAINTENANCE_SCAN_TIMER, MAINT_SCAN_SET);
    s_maintenance.scan_timer_s = seconds;
    return true;
}

bool modem_maintenance_read_scan_timer(void) {
    if (!modem_maintenance_can_admit()) {
        return false;
    }
    maintenance_begin(MODEM_MAINTENANCE_SCAN_TIMER, MAINT_SCAN_QUERY);
    return true;
}

bool modem_maintenance_start_band_test(uint8_t preset) {
    if (preset < 1u || preset > 5u || !modem_maintenance_can_admit()) {
        return false;
    }
    maintenance_begin(MODEM_MAINTENANCE_BAND_TEST, MAINT_BAND_MODE_QUERY);
    s_maintenance.band_preset = preset;
    return true;
}

bool modem_maintenance_restore_band(bool command_in_flight) {
    if (s_maintenance.action != MODEM_MAINTENANCE_BAND_TEST ||
        (!s_maintenance.band_test_active &&
         !s_maintenance.recovery_pending && !s_maintenance_mutated)) {
        return false;
    }
    maintenance_begin_band_restore(false, MODEM_MAINTENANCE_ERROR_NONE,
                                   command_in_flight);
    return true;
}

bool modem_maintenance_enter_antenna(void) {
    if (!modem_maintenance_can_admit()) {
        return false;
    }
    maintenance_begin(MODEM_MAINTENANCE_ANTENNA, MAINT_ANT_SET_CFUN4);
    s_maintenance.antenna_rf = 1u;
    return true;
}

bool modem_maintenance_select_antenna(uint8_t rf_state) {
    if (rf_state < 1u || rf_state > 4u ||
        s_maintenance.action != MODEM_MAINTENANCE_ANTENNA ||
        s_maintenance.state != MODEM_MAINTENANCE_ACTIVE ||
        !s_maintenance.antenna_active) {
        return false;
    }
    s_maintenance.antenna_rf = rf_state;
    maintenance_set_phase(MAINT_ANT_SET_GPIO_A,
                          MODEM_MAINTENANCE_RUNNING);
    return true;
}

bool modem_maintenance_exit_antenna(bool command_in_flight) {
    if (s_maintenance.action != MODEM_MAINTENANCE_ANTENNA ||
        (!s_maintenance.antenna_active && !s_maintenance_mutated)) {
        return false;
    }
    maintenance_begin_antenna_restore(false, MODEM_MAINTENANCE_ERROR_NONE,
                                      command_in_flight);
    return true;
}

void modem_maintenance_cancel(bool command_in_flight) {
    if (s_maintenance.action == MODEM_MAINTENANCE_BAND_TEST) {
        if (s_maintenance.recovery_pending || s_maintenance_mutated ||
            s_maintenance.band_test_active) {
            maintenance_begin_band_restore(false,
                                            MODEM_MAINTENANCE_ERROR_NONE,
                                            command_in_flight);
        } else if (s_maintenance.state == MODEM_MAINTENANCE_PENDING ||
                   s_maintenance.state == MODEM_MAINTENANCE_RUNNING) {
            s_maintenance_cancel_requested = true;
        }
    } else if (s_maintenance.action == MODEM_MAINTENANCE_ANTENNA) {
        if (s_maintenance_mutated || s_maintenance.antenna_active) {
            maintenance_begin_antenna_restore(false,
                                               MODEM_MAINTENANCE_ERROR_NONE,
                                               command_in_flight);
        } else if (s_maintenance.state == MODEM_MAINTENANCE_PENDING ||
                   s_maintenance.state == MODEM_MAINTENANCE_RUNNING) {
            s_maintenance_cancel_requested = true;
        }
    }
}

void modem_maintenance_get_snapshot(modem_maintenance_snapshot_t *out) {
    if (out != NULL) {
        *out = s_maintenance;
    }
}

bool modem_maintenance_recovery_checked(void) {
    return s_maintenance_recovery_checked;
}

modem_maintenance_recovery_result_t modem_maintenance_recover_record(
    const char *record, bool command_in_flight) {
    s_maintenance_recovery_checked = true;
    if (record == NULL || record[0] == '\0') {
        return MODEM_MAINTENANCE_RECOVERY_EMPTY;
    }
    if (!maintenance_record_parse(record, &s_maintenance_original_band_mode,
                                  &s_maintenance_original_band)) {
        s_maintenance.action = MODEM_MAINTENANCE_BAND_TEST;
        s_maintenance.recovery_pending = true;
        maintenance_begin_fail_closed(MODEM_MAINTENANCE_ERROR_STORAGE);
        return MODEM_MAINTENANCE_RECOVERY_FAIL_CLOSED;
    }
    s_maintenance.action = MODEM_MAINTENANCE_BAND_TEST;
    s_maintenance.recovery_pending = true;
    s_maintenance_mutated = true;
    maintenance_begin_band_restore(false, MODEM_MAINTENANCE_ERROR_NONE,
                                   command_in_flight);
    return MODEM_MAINTENANCE_RECOVERY_RESTORE_STARTED;
}

bool modem_maintenance_has_sequence(void) {
    return s_maintenance_phase != MAINT_PHASE_NONE &&
           (s_maintenance.state == MODEM_MAINTENANCE_PENDING ||
            s_maintenance.state == MODEM_MAINTENANCE_RUNNING ||
            s_maintenance.state == MODEM_MAINTENANCE_RESTORING);
}

bool modem_maintenance_blocks_other_work(void) {
    return (s_maintenance.state == MODEM_MAINTENANCE_ACTIVE &&
            s_maintenance.action == MODEM_MAINTENANCE_ANTENNA) ||
           s_maintenance.state == MODEM_MAINTENANCE_LOCKED;
}

bool modem_maintenance_blocks_sleep(void) {
    return s_maintenance.recovery_pending ||
           (s_maintenance.state != MODEM_MAINTENANCE_IDLE &&
            s_maintenance.state != MODEM_MAINTENANCE_DONE &&
            s_maintenance.state != MODEM_MAINTENANCE_ERROR);
}

bool modem_maintenance_holds_cfun4(void) {
    if (!s_maintenance_mutated) {
        return false;
    }
    return s_maintenance.action == MODEM_MAINTENANCE_ANTENNA ||
           s_maintenance_phase == MAINT_FAIL_CLOSED_SET_CFUN4 ||
           s_maintenance_phase == MAINT_FAIL_CLOSED_VERIFY_CFUN4 ||
           s_maintenance.state == MODEM_MAINTENANCE_LOCKED;
}

static bool maintenance_phase_uses_readback(
    modem_maintenance_phase_t phase) {
    switch (phase) {
    case MAINT_SCAN_QUERY:
    case MAINT_SCAN_VERIFY:
    case MAINT_BAND_MODE_QUERY:
    case MAINT_BAND_BASE_QUERY:
    case MAINT_BAND_VERIFY_RAM_MODE:
    case MAINT_BAND_VERIFY_PRESET:
    case MAINT_BAND_RESTORE_VERIFY_RAM_MODE:
    case MAINT_BAND_RESTORE_VERIFY_CONFIG:
    case MAINT_BAND_RESTORE_VERIFY_MODE:
    case MAINT_ANT_VERIFY_CFUN4:
    case MAINT_ANT_VERIFY_TUNER_ENABLED:
    case MAINT_ANT_VERIFY_TUNER_TABLE:
    case MAINT_ANT_VERIFY_GPIO_A_ALT:
    case MAINT_ANT_VERIFY_GPIO_B_ALT:
    case MAINT_ANT_VERIFY_TUNER_DISABLED:
    case MAINT_ANT_VERIFY_GPIO_A_INPUT:
    case MAINT_ANT_VERIFY_GPIO_B_INPUT:
    case MAINT_ANT_VERIFY_GPIO_A:
    case MAINT_ANT_VERIFY_GPIO_B:
    case MAINT_ANT_RESTORE_VERIFY_GPIO_A_INPUT:
    case MAINT_ANT_RESTORE_VERIFY_GPIO_B_INPUT:
    case MAINT_ANT_RESTORE_VERIFY_GPIO_A_ALT:
    case MAINT_ANT_RESTORE_VERIFY_GPIO_B_ALT:
    case MAINT_ANT_RESTORE_VERIFY_TUNER_DISABLED:
    case MAINT_ANT_RESTORE_VERIFY_TABLE:
    case MAINT_ANT_RESTORE_VERIFY_ENABLED:
    case MAINT_ANT_RESTORE_VERIFY_CFUN5:
    case MAINT_FAIL_CLOSED_VERIFY_CFUN4:
        return true;
    case MAINT_PHASE_NONE:
    case MAINT_SCAN_SET:
    case MAINT_BAND_SET_RAM_MODE:
    case MAINT_BAND_SET_PRESET:
    case MAINT_BAND_RESTORE_SET_RAM_MODE:
    case MAINT_BAND_RESTORE_CONFIG:
    case MAINT_BAND_RESTORE_MODE:
    case MAINT_ANT_SET_CFUN4:
    case MAINT_ANT_DISABLE_TUNER:
    case MAINT_ANT_RELEASE_GPIO_A:
    case MAINT_ANT_RELEASE_GPIO_B:
    case MAINT_ANT_SET_GPIO_A:
    case MAINT_ANT_SET_GPIO_B:
    case MAINT_ANT_RESTORE_RELEASE_GPIO_A:
    case MAINT_ANT_RESTORE_RELEASE_GPIO_B:
    case MAINT_ANT_RESTORE_GPIO_A_ALT:
    case MAINT_ANT_RESTORE_GPIO_B_ALT:
    case MAINT_ANT_RESTORE_DISABLE_TUNER:
    case MAINT_ANT_RESTORE_ROW_RF2:
    case MAINT_ANT_RESTORE_ROW_RF3:
    case MAINT_ANT_RESTORE_ROW_RF4:
    case MAINT_ANT_RESTORE_ROW_RF1:
    case MAINT_ANT_RESTORE_SET_CFUN5:
    case MAINT_FAIL_CLOSED_SET_CFUN4:
        return false;
    }
    return false;
}

static bool maintenance_readback_matches(void) {
    if (!s_maintenance_line_seen || s_maintenance_line_invalid) {
        return false;
    }
    switch (s_maintenance_phase) {
    case MAINT_SCAN_QUERY:
    case MAINT_BAND_MODE_QUERY:
    case MAINT_BAND_BASE_QUERY:
        return true;
    case MAINT_SCAN_VERIFY:
        return s_maintenance_observed_u16 == s_maintenance_expected_u16;
    case MAINT_BAND_VERIFY_RAM_MODE:
    case MAINT_BAND_RESTORE_VERIFY_RAM_MODE:
    case MAINT_BAND_RESTORE_VERIFY_MODE:
    case MAINT_ANT_VERIFY_CFUN4:
    case MAINT_ANT_RESTORE_VERIFY_CFUN5:
    case MAINT_FAIL_CLOSED_VERIFY_CFUN4:
        return s_maintenance_observed_u8 == s_maintenance_expected_u8;
    case MAINT_BAND_VERIFY_PRESET:
    case MAINT_BAND_RESTORE_VERIFY_CONFIG:
        return maintenance_band_equal(&s_maintenance_observed_band,
                                      &s_maintenance_expected_band);
    case MAINT_ANT_VERIFY_TUNER_ENABLED:
    case MAINT_ANT_VERIFY_TUNER_DISABLED:
    case MAINT_ANT_RESTORE_VERIFY_TUNER_DISABLED:
    case MAINT_ANT_RESTORE_VERIFY_ENABLED:
        return s_maintenance_observed_bool ==
               (s_maintenance_expected_u8 != 0u);
    case MAINT_ANT_VERIFY_TUNER_TABLE:
    case MAINT_ANT_RESTORE_VERIFY_TABLE:
        return s_backend != NULL &&
               s_backend->tuner_table_exact != NULL &&
               s_backend->tuner_table_exact();
    case MAINT_ANT_VERIFY_GPIO_A_ALT:
    case MAINT_ANT_VERIFY_GPIO_B_ALT:
    case MAINT_ANT_VERIFY_GPIO_A_INPUT:
    case MAINT_ANT_VERIFY_GPIO_B_INPUT:
    case MAINT_ANT_VERIFY_GPIO_A:
    case MAINT_ANT_VERIFY_GPIO_B:
    case MAINT_ANT_RESTORE_VERIFY_GPIO_A_INPUT:
    case MAINT_ANT_RESTORE_VERIFY_GPIO_B_INPUT:
    case MAINT_ANT_RESTORE_VERIFY_GPIO_A_ALT:
    case MAINT_ANT_RESTORE_VERIFY_GPIO_B_ALT:
        return s_maintenance_observed_gpio.direction ==
                   s_maintenance_expected_gpio.direction &&
               (s_maintenance_expected_gpio.direction != 1u ||
                s_maintenance_observed_gpio.state ==
                    s_maintenance_expected_gpio.state);
    default:
        return true;
    }
}

static void maintenance_failure(modem_maintenance_error_t error) {
    if (s_maintenance.retries < MODEM_MAINTENANCE_RETRY_LIMIT) {
        s_maintenance.retries++;
        s_maintenance.error = error;
        s_maintenance.sequence++;
        return;
    }

    bool band_restore = maintenance_phase_is_band_restore(
        s_maintenance_phase);
    bool antenna_restore = maintenance_phase_is_antenna_restore(
        s_maintenance_phase);
    if (s_maintenance_phase == MAINT_FAIL_CLOSED_SET_CFUN4 ||
        s_maintenance_phase == MAINT_FAIL_CLOSED_VERIFY_CFUN4) {
        s_maintenance_phase = MAINT_PHASE_NONE;
        s_maintenance.retries = 0u;
        maintenance_publish(MODEM_MAINTENANCE_LOCKED, error);
        return;
    }
    if (s_maintenance.action == MODEM_MAINTENANCE_BAND_TEST &&
        (s_maintenance_mutated || s_maintenance.recovery_pending)) {
        if (band_restore) {
            maintenance_begin_fail_closed(error);
        } else {
            maintenance_begin_band_restore(true, error, false);
        }
        return;
    }
    if (s_maintenance.action == MODEM_MAINTENANCE_ANTENNA &&
        s_maintenance_mutated) {
        if (antenna_restore) {
            maintenance_begin_fail_closed(error);
        } else {
            maintenance_begin_antenna_restore(true, error, false);
        }
        return;
    }
    maintenance_finish_terminal(MODEM_MAINTENANCE_ERROR, error);
}

static void maintenance_complete_restore(void) {
    modem_maintenance_error_t error = s_maintenance_finishes_error
        ? s_maintenance_original_error : MODEM_MAINTENANCE_ERROR_NONE;
    modem_maintenance_state_t state = s_maintenance_finishes_error
        ? MODEM_MAINTENANCE_ERROR : MODEM_MAINTENANCE_DONE;
    s_maintenance.band_test_active = false;
    s_maintenance.band_preset = 0u;
    s_maintenance.antenna_active = false;
    s_maintenance.antenna_rf = 0u;
    maintenance_finish_terminal(state, error);
}

static void maintenance_success(void) {
    switch (s_maintenance_phase) {
    case MAINT_SCAN_QUERY:
        s_maintenance.scan_timer_s = s_maintenance_observed_u16;
        maintenance_finish_terminal(MODEM_MAINTENANCE_DONE,
                                    MODEM_MAINTENANCE_ERROR_NONE);
        break;
    case MAINT_SCAN_SET:
        maintenance_set_phase(MAINT_SCAN_VERIFY,
                              MODEM_MAINTENANCE_RUNNING);
        break;
    case MAINT_SCAN_VERIFY:
        s_maintenance.scan_timer_s = s_maintenance_observed_u16;
        maintenance_finish_terminal(MODEM_MAINTENANCE_DONE,
                                    MODEM_MAINTENANCE_ERROR_NONE);
        break;
    case MAINT_BAND_MODE_QUERY:
        s_maintenance_original_band_mode = s_maintenance_observed_u8;
        maintenance_set_phase(MAINT_BAND_BASE_QUERY,
                              MODEM_MAINTENANCE_RUNNING);
        break;
    case MAINT_BAND_BASE_QUERY:
        s_maintenance_original_band = s_maintenance_observed_band;
        if (s_backend == NULL || s_backend->band_preset == NULL ||
            !s_backend->band_preset(
                s_maintenance.band_preset, &s_maintenance_original_band,
                &s_maintenance_target_band)) {
            maintenance_finish_terminal(MODEM_MAINTENANCE_ERROR,
                                        MODEM_MAINTENANCE_ERROR_ARGUMENT);
        } else if (!maintenance_record_store()) {
            maintenance_finish_terminal(MODEM_MAINTENANCE_ERROR,
                                        MODEM_MAINTENANCE_ERROR_STORAGE);
        } else {
            maintenance_set_phase(MAINT_BAND_SET_RAM_MODE,
                                  MODEM_MAINTENANCE_RUNNING);
        }
        break;
    case MAINT_BAND_SET_RAM_MODE:
        maintenance_set_phase(MAINT_BAND_VERIFY_RAM_MODE,
                              MODEM_MAINTENANCE_RUNNING);
        break;
    case MAINT_BAND_VERIFY_RAM_MODE:
        maintenance_set_phase(MAINT_BAND_SET_PRESET,
                              MODEM_MAINTENANCE_RUNNING);
        break;
    case MAINT_BAND_SET_PRESET:
        maintenance_set_phase(MAINT_BAND_VERIFY_PRESET,
                              MODEM_MAINTENANCE_RUNNING);
        break;
    case MAINT_BAND_VERIFY_PRESET:
        s_maintenance.band_test_active = true;
        s_maintenance_phase = MAINT_PHASE_NONE;
        maintenance_publish(MODEM_MAINTENANCE_ACTIVE,
                            MODEM_MAINTENANCE_ERROR_NONE);
        break;
    case MAINT_BAND_RESTORE_SET_RAM_MODE:
        maintenance_set_phase(MAINT_BAND_RESTORE_VERIFY_RAM_MODE,
                              MODEM_MAINTENANCE_RESTORING);
        break;
    case MAINT_BAND_RESTORE_VERIFY_RAM_MODE:
        maintenance_set_phase(MAINT_BAND_RESTORE_CONFIG,
                              MODEM_MAINTENANCE_RESTORING);
        break;
    case MAINT_BAND_RESTORE_CONFIG:
        maintenance_set_phase(MAINT_BAND_RESTORE_VERIFY_CONFIG,
                              MODEM_MAINTENANCE_RESTORING);
        break;
    case MAINT_BAND_RESTORE_VERIFY_CONFIG:
        maintenance_set_phase(MAINT_BAND_RESTORE_MODE,
                              MODEM_MAINTENANCE_RESTORING);
        break;
    case MAINT_BAND_RESTORE_MODE:
        maintenance_set_phase(MAINT_BAND_RESTORE_VERIFY_MODE,
                              MODEM_MAINTENANCE_RESTORING);
        break;
    case MAINT_BAND_RESTORE_VERIFY_MODE:
        s_maintenance.band_test_active = false;
        if (!maintenance_record_clear()) {
            s_maintenance_phase = MAINT_PHASE_NONE;
            s_maintenance.retries = 0u;
            maintenance_publish(MODEM_MAINTENANCE_ERROR,
                                MODEM_MAINTENANCE_ERROR_STORAGE);
        } else {
            maintenance_complete_restore();
        }
        break;
    case MAINT_ANT_SET_CFUN4:
        maintenance_set_phase(MAINT_ANT_VERIFY_CFUN4,
                              MODEM_MAINTENANCE_RUNNING);
        break;
    case MAINT_ANT_VERIFY_CFUN4:
        maintenance_set_phase(MAINT_ANT_VERIFY_TUNER_ENABLED,
                              MODEM_MAINTENANCE_RUNNING);
        break;
    case MAINT_ANT_VERIFY_TUNER_ENABLED:
        maintenance_set_phase(MAINT_ANT_VERIFY_TUNER_TABLE,
                              MODEM_MAINTENANCE_RUNNING);
        break;
    case MAINT_ANT_VERIFY_TUNER_TABLE:
        maintenance_set_phase(MAINT_ANT_VERIFY_GPIO_A_ALT,
                              MODEM_MAINTENANCE_RUNNING);
        break;
    case MAINT_ANT_VERIFY_GPIO_A_ALT:
        maintenance_set_phase(MAINT_ANT_VERIFY_GPIO_B_ALT,
                              MODEM_MAINTENANCE_RUNNING);
        break;
    case MAINT_ANT_VERIFY_GPIO_B_ALT:
        maintenance_set_phase(MAINT_ANT_DISABLE_TUNER,
                              MODEM_MAINTENANCE_RUNNING);
        break;
    case MAINT_ANT_DISABLE_TUNER:
        maintenance_set_phase(MAINT_ANT_VERIFY_TUNER_DISABLED,
                              MODEM_MAINTENANCE_RUNNING);
        break;
    case MAINT_ANT_VERIFY_TUNER_DISABLED:
        maintenance_set_phase(MAINT_ANT_RELEASE_GPIO_A,
                              MODEM_MAINTENANCE_RUNNING);
        break;
    case MAINT_ANT_RELEASE_GPIO_A:
        maintenance_set_phase(MAINT_ANT_VERIFY_GPIO_A_INPUT,
                              MODEM_MAINTENANCE_RUNNING);
        break;
    case MAINT_ANT_VERIFY_GPIO_A_INPUT:
        maintenance_set_phase(MAINT_ANT_RELEASE_GPIO_B,
                              MODEM_MAINTENANCE_RUNNING);
        break;
    case MAINT_ANT_RELEASE_GPIO_B:
        maintenance_set_phase(MAINT_ANT_VERIFY_GPIO_B_INPUT,
                              MODEM_MAINTENANCE_RUNNING);
        break;
    case MAINT_ANT_VERIFY_GPIO_B_INPUT:
        maintenance_set_phase(MAINT_ANT_SET_GPIO_A,
                              MODEM_MAINTENANCE_RUNNING);
        break;
    case MAINT_ANT_SET_GPIO_A:
        maintenance_set_phase(MAINT_ANT_VERIFY_GPIO_A,
                              MODEM_MAINTENANCE_RUNNING);
        break;
    case MAINT_ANT_VERIFY_GPIO_A:
        maintenance_set_phase(MAINT_ANT_SET_GPIO_B,
                              MODEM_MAINTENANCE_RUNNING);
        break;
    case MAINT_ANT_SET_GPIO_B:
        maintenance_set_phase(MAINT_ANT_VERIFY_GPIO_B,
                              MODEM_MAINTENANCE_RUNNING);
        break;
    case MAINT_ANT_VERIFY_GPIO_B:
        s_maintenance.antenna_active = true;
        s_maintenance_phase = MAINT_PHASE_NONE;
        maintenance_publish(MODEM_MAINTENANCE_ACTIVE,
                            MODEM_MAINTENANCE_ERROR_NONE);
        break;
    case MAINT_ANT_RESTORE_RELEASE_GPIO_A:
        maintenance_set_phase(MAINT_ANT_RESTORE_VERIFY_GPIO_A_INPUT,
                              MODEM_MAINTENANCE_RESTORING);
        break;
    case MAINT_ANT_RESTORE_VERIFY_GPIO_A_INPUT:
        maintenance_set_phase(MAINT_ANT_RESTORE_RELEASE_GPIO_B,
                              MODEM_MAINTENANCE_RESTORING);
        break;
    case MAINT_ANT_RESTORE_RELEASE_GPIO_B:
        maintenance_set_phase(MAINT_ANT_RESTORE_VERIFY_GPIO_B_INPUT,
                              MODEM_MAINTENANCE_RESTORING);
        break;
    case MAINT_ANT_RESTORE_VERIFY_GPIO_B_INPUT:
        maintenance_set_phase(MAINT_ANT_RESTORE_GPIO_A_ALT,
                              MODEM_MAINTENANCE_RESTORING);
        break;
    case MAINT_ANT_RESTORE_GPIO_A_ALT:
        maintenance_set_phase(MAINT_ANT_RESTORE_VERIFY_GPIO_A_ALT,
                              MODEM_MAINTENANCE_RESTORING);
        break;
    case MAINT_ANT_RESTORE_VERIFY_GPIO_A_ALT:
        maintenance_set_phase(MAINT_ANT_RESTORE_GPIO_B_ALT,
                              MODEM_MAINTENANCE_RESTORING);
        break;
    case MAINT_ANT_RESTORE_GPIO_B_ALT:
        maintenance_set_phase(MAINT_ANT_RESTORE_VERIFY_GPIO_B_ALT,
                              MODEM_MAINTENANCE_RESTORING);
        break;
    case MAINT_ANT_RESTORE_VERIFY_GPIO_B_ALT:
        maintenance_set_phase(MAINT_ANT_RESTORE_DISABLE_TUNER,
                              MODEM_MAINTENANCE_RESTORING);
        break;
    case MAINT_ANT_RESTORE_DISABLE_TUNER:
        maintenance_set_phase(MAINT_ANT_RESTORE_VERIFY_TUNER_DISABLED,
                              MODEM_MAINTENANCE_RESTORING);
        break;
    case MAINT_ANT_RESTORE_VERIFY_TUNER_DISABLED:
        maintenance_set_phase(MAINT_ANT_RESTORE_ROW_RF2,
                              MODEM_MAINTENANCE_RESTORING);
        break;
    case MAINT_ANT_RESTORE_ROW_RF2:
        maintenance_set_phase(MAINT_ANT_RESTORE_ROW_RF3,
                              MODEM_MAINTENANCE_RESTORING);
        break;
    case MAINT_ANT_RESTORE_ROW_RF3:
        maintenance_set_phase(MAINT_ANT_RESTORE_ROW_RF4,
                              MODEM_MAINTENANCE_RESTORING);
        break;
    case MAINT_ANT_RESTORE_ROW_RF4:
        maintenance_set_phase(MAINT_ANT_RESTORE_ROW_RF1,
                              MODEM_MAINTENANCE_RESTORING);
        break;
    case MAINT_ANT_RESTORE_ROW_RF1:
        maintenance_set_phase(MAINT_ANT_RESTORE_VERIFY_TABLE,
                              MODEM_MAINTENANCE_RESTORING);
        break;
    case MAINT_ANT_RESTORE_VERIFY_TABLE:
        maintenance_set_phase(MAINT_ANT_RESTORE_VERIFY_ENABLED,
                              MODEM_MAINTENANCE_RESTORING);
        break;
    case MAINT_ANT_RESTORE_VERIFY_ENABLED:
        maintenance_set_phase(MAINT_ANT_RESTORE_SET_CFUN5,
                              MODEM_MAINTENANCE_RESTORING);
        break;
    case MAINT_ANT_RESTORE_SET_CFUN5:
        maintenance_set_phase(MAINT_ANT_RESTORE_VERIFY_CFUN5,
                              MODEM_MAINTENANCE_RESTORING);
        break;
    case MAINT_ANT_RESTORE_VERIFY_CFUN5:
        maintenance_complete_restore();
        break;
    case MAINT_FAIL_CLOSED_SET_CFUN4:
        maintenance_set_phase(MAINT_FAIL_CLOSED_VERIFY_CFUN4,
                              MODEM_MAINTENANCE_RESTORING);
        break;
    case MAINT_FAIL_CLOSED_VERIFY_CFUN4:
        s_maintenance_phase = MAINT_PHASE_NONE;
        s_maintenance.retries = 0u;
        maintenance_publish(MODEM_MAINTENANCE_LOCKED,
                            s_maintenance_original_error);
        break;
    case MAINT_PHASE_NONE:
        break;
    }
}

static bool maintenance_build_gpio_query(uint8_t pin, uint8_t direction,
                                         bool state, char *command,
                                         size_t command_cap) {
    s_maintenance_expected_gpio.direction = direction;
    s_maintenance_expected_gpio.state = state;
    return s_backend != NULL && s_backend->build_gpio_query != NULL &&
           s_backend->build_gpio_query(pin, command, command_cap);
}

static bool maintenance_build_gpio_set(uint8_t pin, uint8_t direction,
                                       bool state, char *command,
                                       size_t command_cap) {
    return s_backend != NULL && s_backend->build_gpio_set != NULL &&
           s_backend->build_gpio_set(pin, state, direction, false,
                                     command, command_cap);
}

bool modem_maintenance_prepare_next(uint32_t now_ms,
                                    modem_maintenance_dispatch_t *out) {
    if (out == NULL || s_backend == NULL ||
        !modem_maintenance_has_sequence()) {
        return false;
    }

    if (s_maintenance_cancel_requested && !s_maintenance_mutated) {
        if (!s_maintenance.recovery_pending || maintenance_record_clear()) {
            maintenance_finish_terminal(MODEM_MAINTENANCE_DONE,
                                        MODEM_MAINTENANCE_ERROR_CANCELLED);
        } else {
            maintenance_finish_terminal(MODEM_MAINTENANCE_ERROR,
                                        MODEM_MAINTENANCE_ERROR_STORAGE);
        }
        return false;
    }

    const char *selected = NULL;
    bool built = true;
    bool control_a = false;
    bool control_b = false;
    memset(out, 0, sizeof(*out));
    out->timeout_ms = MODEM_MAINTENANCE_TIMEOUT_MS;
    s_maintenance_line_seen = false;
    s_maintenance_line_invalid = false;
    memset(&s_maintenance_observed_band, 0,
           sizeof(s_maintenance_observed_band));
    memset(&s_maintenance_observed_gpio, 0,
           sizeof(s_maintenance_observed_gpio));

    switch (s_maintenance_phase) {
    case MAINT_SCAN_QUERY:
    case MAINT_SCAN_VERIFY:
        selected = s_backend->scan_timer_query_cmd;
        s_maintenance_expected_u16 = s_maintenance.scan_timer_s;
        break;
    case MAINT_SCAN_SET:
        built = s_backend->build_scan_timer_set != NULL &&
            s_backend->build_scan_timer_set(s_maintenance.scan_timer_s,
                                            out->command,
                                            sizeof(out->command));
        break;
    case MAINT_BAND_MODE_QUERY:
    case MAINT_BAND_VERIFY_RAM_MODE:
    case MAINT_BAND_RESTORE_VERIFY_RAM_MODE:
    case MAINT_BAND_RESTORE_VERIFY_MODE:
        selected = s_backend->band_mode_query_cmd;
        s_maintenance_expected_u8 =
            s_maintenance_phase == MAINT_BAND_RESTORE_VERIFY_MODE
                ? s_maintenance_original_band_mode : 1u;
        break;
    case MAINT_BAND_BASE_QUERY:
        selected = s_maintenance_original_band_mode == 0u
            ? s_backend->band_nvm_query_cmd : s_backend->band_ram_query_cmd;
        break;
    case MAINT_BAND_SET_RAM_MODE:
    case MAINT_BAND_RESTORE_SET_RAM_MODE:
        s_maintenance_mutated = true;
        built = s_backend->build_band_mode_set != NULL &&
            s_backend->build_band_mode_set(1u, out->command,
                                           sizeof(out->command));
        break;
    case MAINT_BAND_SET_PRESET:
        s_maintenance_mutated = true;
        built = s_backend->build_band_ram_set != NULL &&
            s_backend->build_band_ram_set(&s_maintenance_target_band,
                                          out->command,
                                          sizeof(out->command));
        break;
    case MAINT_BAND_VERIFY_PRESET:
        selected = s_backend->band_ram_query_cmd;
        s_maintenance_expected_band = s_maintenance_target_band;
        break;
    case MAINT_BAND_RESTORE_CONFIG:
        s_maintenance_mutated = true;
        built = s_backend->build_band_ram_set != NULL &&
            s_backend->build_band_ram_set(&s_maintenance_original_band,
                                          out->command,
                                          sizeof(out->command));
        break;
    case MAINT_BAND_RESTORE_VERIFY_CONFIG:
        selected = s_backend->band_ram_query_cmd;
        s_maintenance_expected_band = s_maintenance_original_band;
        break;
    case MAINT_BAND_RESTORE_MODE:
        s_maintenance_mutated = true;
        built = s_backend->build_band_mode_set != NULL &&
            s_backend->build_band_mode_set(s_maintenance_original_band_mode,
                                           out->command,
                                           sizeof(out->command));
        break;
    case MAINT_ANT_SET_CFUN4:
    case MAINT_FAIL_CLOSED_SET_CFUN4:
        s_maintenance_mutated = true;
        if (s_hooks.hold_sim_offline != NULL) {
            s_hooks.hold_sim_offline();
        }
        built = s_backend->build_function_set != NULL &&
            s_backend->build_function_set(4u, out->command,
                                          sizeof(out->command));
        break;
    case MAINT_ANT_VERIFY_CFUN4:
    case MAINT_FAIL_CLOSED_VERIFY_CFUN4:
        selected = s_backend->function_query_cmd;
        s_maintenance_expected_u8 = 4u;
        break;
    case MAINT_ANT_VERIFY_TUNER_ENABLED:
    case MAINT_ANT_RESTORE_VERIFY_ENABLED:
        selected = s_backend->tuner_enabled_query_cmd;
        s_maintenance_expected_u8 = 1u;
        break;
    case MAINT_ANT_VERIFY_TUNER_TABLE:
    case MAINT_ANT_RESTORE_VERIFY_TABLE:
        if (s_backend->tuner_table_begin != NULL) {
            s_backend->tuner_table_begin();
        }
        selected = s_backend->tuner_table_query_cmd;
        break;
    case MAINT_ANT_VERIFY_GPIO_A_ALT:
    case MAINT_ANT_RESTORE_VERIFY_GPIO_A_ALT:
        built = maintenance_build_gpio_query(
            s_backend->antenna_gpio_a,
            s_backend->antenna_gpio_a_alt_direction,
            false, out->command, sizeof(out->command));
        break;
    case MAINT_ANT_VERIFY_GPIO_B_ALT:
    case MAINT_ANT_RESTORE_VERIFY_GPIO_B_ALT:
        built = maintenance_build_gpio_query(
            s_backend->antenna_gpio_b,
            s_backend->antenna_gpio_b_alt_direction,
            false, out->command, sizeof(out->command));
        break;
    case MAINT_ANT_DISABLE_TUNER:
    case MAINT_ANT_RESTORE_DISABLE_TUNER:
        s_maintenance_mutated = true;
        built = s_backend->build_tuner_enabled_set != NULL &&
            s_backend->build_tuner_enabled_set(false, out->command,
                                               sizeof(out->command));
        break;
    case MAINT_ANT_VERIFY_TUNER_DISABLED:
    case MAINT_ANT_RESTORE_VERIFY_TUNER_DISABLED:
        selected = s_backend->tuner_enabled_query_cmd;
        s_maintenance_expected_u8 = 0u;
        break;
    case MAINT_ANT_RELEASE_GPIO_A:
    case MAINT_ANT_RESTORE_RELEASE_GPIO_A:
        s_maintenance_mutated = true;
        built = maintenance_build_gpio_set(s_backend->antenna_gpio_a, 0u,
                                           false, out->command,
                                           sizeof(out->command));
        break;
    case MAINT_ANT_RELEASE_GPIO_B:
    case MAINT_ANT_RESTORE_RELEASE_GPIO_B:
        s_maintenance_mutated = true;
        built = maintenance_build_gpio_set(s_backend->antenna_gpio_b, 0u,
                                           false, out->command,
                                           sizeof(out->command));
        break;
    case MAINT_ANT_VERIFY_GPIO_A_INPUT:
    case MAINT_ANT_RESTORE_VERIFY_GPIO_A_INPUT:
        built = maintenance_build_gpio_query(s_backend->antenna_gpio_a, 0u,
                                             false, out->command,
                                             sizeof(out->command));
        break;
    case MAINT_ANT_VERIFY_GPIO_B_INPUT:
    case MAINT_ANT_RESTORE_VERIFY_GPIO_B_INPUT:
        built = maintenance_build_gpio_query(s_backend->antenna_gpio_b, 0u,
                                             false, out->command,
                                             sizeof(out->command));
        break;
    case MAINT_ANT_SET_GPIO_A:
        built = s_backend->antenna_controls != NULL &&
            s_backend->antenna_controls(s_maintenance.antenna_rf,
                                        &control_a, &control_b) &&
            maintenance_build_gpio_set(s_backend->antenna_gpio_a, 1u,
                                       control_a, out->command,
                                       sizeof(out->command));
        break;
    case MAINT_ANT_VERIFY_GPIO_A:
        built = s_backend->antenna_controls != NULL &&
            s_backend->antenna_controls(s_maintenance.antenna_rf,
                                        &control_a, &control_b) &&
            maintenance_build_gpio_query(s_backend->antenna_gpio_a, 1u,
                                         control_a, out->command,
                                         sizeof(out->command));
        break;
    case MAINT_ANT_SET_GPIO_B:
        built = s_backend->antenna_controls != NULL &&
            s_backend->antenna_controls(s_maintenance.antenna_rf,
                                        &control_a, &control_b) &&
            maintenance_build_gpio_set(s_backend->antenna_gpio_b, 1u,
                                       control_b, out->command,
                                       sizeof(out->command));
        break;
    case MAINT_ANT_VERIFY_GPIO_B:
        built = s_backend->antenna_controls != NULL &&
            s_backend->antenna_controls(s_maintenance.antenna_rf,
                                        &control_a, &control_b) &&
            maintenance_build_gpio_query(s_backend->antenna_gpio_b, 1u,
                                         control_b, out->command,
                                         sizeof(out->command));
        break;
    case MAINT_ANT_RESTORE_GPIO_A_ALT:
        s_maintenance_mutated = true;
        built = maintenance_build_gpio_set(
            s_backend->antenna_gpio_a,
            s_backend->antenna_gpio_a_alt_direction,
            false, out->command, sizeof(out->command));
        break;
    case MAINT_ANT_RESTORE_GPIO_B_ALT:
        s_maintenance_mutated = true;
        built = maintenance_build_gpio_set(
            s_backend->antenna_gpio_b,
            s_backend->antenna_gpio_b_alt_direction,
            false, out->command, sizeof(out->command));
        break;
    case MAINT_ANT_RESTORE_ROW_RF2:
    case MAINT_ANT_RESTORE_ROW_RF3:
    case MAINT_ANT_RESTORE_ROW_RF4:
    case MAINT_ANT_RESTORE_ROW_RF1: {
        static const uint8_t RF_FOR_PHASE[] = {2u, 3u, 4u, 1u};
        unsigned ordinal = (unsigned)s_maintenance_phase -
                           (unsigned)MAINT_ANT_RESTORE_ROW_RF2;
        s_maintenance_mutated = true;
        built = ordinal < sizeof(RF_FOR_PHASE) &&
            s_backend->build_tuner_row != NULL &&
            s_backend->build_tuner_row(RF_FOR_PHASE[ordinal], out->command,
                                       sizeof(out->command));
        break;
    }
    case MAINT_ANT_RESTORE_SET_CFUN5:
        s_maintenance_mutated = true;
        if (s_hooks.begin_sim_online != NULL) {
            s_hooks.begin_sim_online(now_ms);
        }
        built = s_backend->build_function_set != NULL &&
            s_backend->build_function_set(5u, out->command,
                                          sizeof(out->command));
        break;
    case MAINT_ANT_RESTORE_VERIFY_CFUN5:
        selected = s_backend->function_query_cmd;
        s_maintenance_expected_u8 = 5u;
        break;
    case MAINT_PHASE_NONE:
        return false;
    }

    if (selected != NULL) {
        size_t length = strlen(selected);
        built = length != 0u && length < sizeof(out->command);
        if (built) {
            memcpy(out->command, selected, length + 1u);
        }
    }
    if (!built || out->command[0] == '\0') {
        maintenance_failure(MODEM_MAINTENANCE_ERROR_UNAVAILABLE);
        return false;
    }
    if (s_maintenance.state == MODEM_MAINTENANCE_PENDING) {
        s_maintenance.state = MODEM_MAINTENANCE_RUNNING;
        s_maintenance.sequence++;
    }
    return true;
}

bool modem_maintenance_parse_expected_line(const char *line) {
    if (line == NULL || s_backend == NULL) {
        return false;
    }
    modem_diag_line_result_t result = MODEM_DIAG_LINE_IGNORE;
    switch (s_maintenance_phase) {
    case MAINT_SCAN_QUERY:
    case MAINT_SCAN_VERIFY:
        if (s_backend->parse_scan_timer != NULL) {
            result = s_backend->parse_scan_timer(
                line, &s_maintenance_observed_u16);
        }
        break;
    case MAINT_BAND_MODE_QUERY:
    case MAINT_BAND_VERIFY_RAM_MODE:
    case MAINT_BAND_RESTORE_VERIFY_RAM_MODE:
    case MAINT_BAND_RESTORE_VERIFY_MODE:
        if (s_backend->parse_band_mode != NULL) {
            result = s_backend->parse_band_mode(
                line, &s_maintenance_observed_u8);
        }
        break;
    case MAINT_BAND_BASE_QUERY:
        if (s_maintenance_original_band_mode == 0u &&
            s_backend->parse_band_nvm != NULL) {
            result = s_backend->parse_band_nvm(
                line, &s_maintenance_observed_band);
        } else if (s_maintenance_original_band_mode == 1u &&
                   s_backend->parse_band_ram != NULL) {
            result = s_backend->parse_band_ram(
                line, &s_maintenance_observed_band);
        }
        break;
    case MAINT_BAND_VERIFY_PRESET:
    case MAINT_BAND_RESTORE_VERIFY_CONFIG:
        if (s_backend->parse_band_ram != NULL) {
            result = s_backend->parse_band_ram(
                line, &s_maintenance_observed_band);
        }
        break;
    case MAINT_ANT_VERIFY_CFUN4:
    case MAINT_ANT_RESTORE_VERIFY_CFUN5:
    case MAINT_FAIL_CLOSED_VERIFY_CFUN4:
        if (s_backend->parse_function != NULL) {
            result = s_backend->parse_function(
                line, &s_maintenance_observed_u8);
        }
        break;
    case MAINT_ANT_VERIFY_TUNER_ENABLED:
    case MAINT_ANT_VERIFY_TUNER_DISABLED:
    case MAINT_ANT_RESTORE_VERIFY_TUNER_DISABLED:
    case MAINT_ANT_RESTORE_VERIFY_ENABLED:
        if (s_backend->parse_tuner_enabled != NULL) {
            result = s_backend->parse_tuner_enabled(
                line, &s_maintenance_observed_bool);
        }
        break;
    case MAINT_ANT_VERIFY_TUNER_TABLE:
    case MAINT_ANT_RESTORE_VERIFY_TABLE:
        if (s_backend->parse_tuner_table_row != NULL) {
            result = s_backend->parse_tuner_table_row(line);
        }
        break;
    case MAINT_ANT_VERIFY_GPIO_A_ALT:
    case MAINT_ANT_VERIFY_GPIO_B_ALT:
    case MAINT_ANT_VERIFY_GPIO_A_INPUT:
    case MAINT_ANT_VERIFY_GPIO_B_INPUT:
    case MAINT_ANT_VERIFY_GPIO_A:
    case MAINT_ANT_VERIFY_GPIO_B:
    case MAINT_ANT_RESTORE_VERIFY_GPIO_A_INPUT:
    case MAINT_ANT_RESTORE_VERIFY_GPIO_B_INPUT:
    case MAINT_ANT_RESTORE_VERIFY_GPIO_A_ALT:
    case MAINT_ANT_RESTORE_VERIFY_GPIO_B_ALT:
        if (s_backend->parse_gpio != NULL) {
            result = s_backend->parse_gpio(
                line, &s_maintenance_observed_gpio);
        }
        break;
    default:
        break;
    }
    if (result == MODEM_DIAG_LINE_ACCEPT) {
        s_maintenance_line_seen = true;
    } else if (result == MODEM_DIAG_LINE_INVALID) {
        s_maintenance_line_invalid = true;
    }
    return result != MODEM_DIAG_LINE_IGNORE;
}

void modem_maintenance_finish_command(bool ok, bool timed_out,
                                      uint32_t now_ms) {
    (void)now_ms;
    if (s_maintenance_phase == MAINT_PHASE_NONE) {
        return;
    }
    bool valid = ok &&
        (!maintenance_phase_uses_readback(s_maintenance_phase) ||
         maintenance_readback_matches());
    if (!valid) {
        modem_maintenance_error_t error = timed_out
            ? MODEM_MAINTENANCE_ERROR_TIMEOUT
            : (!ok ? MODEM_MAINTENANCE_ERROR_COMMAND
                   : MODEM_MAINTENANCE_ERROR_READBACK);
        maintenance_failure(error);
        return;
    }
    s_maintenance.error = MODEM_MAINTENANCE_ERROR_NONE;
    maintenance_success();
    if (s_maintenance_cancel_requested && s_maintenance_mutated) {
        if (s_maintenance.action == MODEM_MAINTENANCE_BAND_TEST) {
            maintenance_begin_band_restore(false,
                                            MODEM_MAINTENANCE_ERROR_NONE,
                                            false);
        } else if (s_maintenance.action == MODEM_MAINTENANCE_ANTENNA) {
            maintenance_begin_antenna_restore(false,
                                               MODEM_MAINTENANCE_ERROR_NONE,
                                               false);
        }
    }
}

void modem_maintenance_cancel_for_call(bool command_in_flight) {
    if (s_maintenance.state == MODEM_MAINTENANCE_IDLE ||
        s_maintenance.state == MODEM_MAINTENANCE_DONE ||
        s_maintenance.state == MODEM_MAINTENANCE_ERROR ||
        s_maintenance.state == MODEM_MAINTENANCE_LOCKED) {
        return;
    }
    if (s_maintenance.action == MODEM_MAINTENANCE_SCAN_TIMER) {
        /* The timer itself is harmless and has no rollback value. Once the
         * current final is consumed, skip any pending verify so a call cannot
         * wait behind another maintenance command. */
        s_maintenance_cancel_requested = true;
    } else if (s_maintenance.action == MODEM_MAINTENANCE_BAND_TEST) {
        if (s_maintenance_mutated || s_maintenance.recovery_pending ||
            s_maintenance.band_test_active) {
            maintenance_begin_band_restore(false,
                                            MODEM_MAINTENANCE_ERROR_NONE,
                                            command_in_flight);
        } else {
            s_maintenance_cancel_requested = true;
        }
    } else if (s_maintenance.action == MODEM_MAINTENANCE_ANTENNA) {
        if (s_maintenance_mutated || s_maintenance.antenna_active) {
            maintenance_begin_antenna_restore(false,
                                               MODEM_MAINTENANCE_ERROR_NONE,
                                               command_in_flight);
        } else {
            s_maintenance_cancel_requested = true;
        }
    }
}
