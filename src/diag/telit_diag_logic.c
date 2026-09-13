#include "diag/telit_diag_logic.h"

#include <stddef.h>

static bool deadline_reached(uint32_t now_ms, uint32_t deadline_ms) {
    return (int32_t)(now_ms - deadline_ms) >= 0;
}

static void clear_transient_controls(telit_diag_logic_t *logic) {
    logic->pulse = TELIT_DIAG_PULSE_NONE;
    logic->pulse_width_ms = 0u;
    logic->pulse_deadline_ms = 0u;
    logic->emergency_armed = false;
    logic->emergency_arm_deadline_ms = 0u;
}

static void clear_status_low_tracking(telit_diag_logic_t *logic) {
    logic->status_low_tracking = false;
    logic->status_low_stable = false;
    logic->status_low_since_ms = 0u;
    logic->status_last_sample_ms = 0u;
}

static telit_diag_result_t start_pulse(
    telit_diag_logic_t *logic, uint32_t now_ms,
    telit_diag_observation_t observation, telit_diag_pulse_t pulse,
    uint32_t width_ms) {
    if (logic == NULL || !logic->rail_enabled ||
        logic->pulse != TELIT_DIAG_PULSE_NONE) {
        return TELIT_DIAG_ERR_STATE;
    }
    if (!observation.power_good) {
        return TELIT_DIAG_ERR_NEEDS_PG;
    }
    logic->dtr_sleep_permitted = false;
    logic->emergency_armed = false;
    logic->emergency_arm_deadline_ms = 0u;
    clear_status_low_tracking(logic);
    logic->pulse = pulse;
    logic->pulse_width_ms = width_ms;
    logic->pulse_deadline_ms = now_ms + width_ms;
    logic->pulse_sequence++;
    if (logic->pulse_sequence == 0u) {
        logic->pulse_sequence++;
    }
    return TELIT_DIAG_OK;
}

void telit_diag_logic_init(telit_diag_logic_t *logic) {
    if (logic == NULL) {
        return;
    }
    *logic = (telit_diag_logic_t){0};
}

void telit_diag_logic_tick(telit_diag_logic_t *logic, uint32_t now_ms,
                           telit_diag_observation_t observation) {
    if (logic == NULL) {
        return;
    }
    if (!logic->rail_enabled) {
        logic->force_pwm = false;
        logic->dtr_sleep_permitted = false;
        logic->boot_attempted = false;
        clear_status_low_tracking(logic);
        clear_transient_controls(logic);
        return;
    }
    bool sample_gap =
        logic->status_low_tracking &&
        (uint32_t)(now_ms - logic->status_last_sample_ms) >
            TELIT_DIAG_STATUS_SAMPLE_MAX_GAP_MS;
    if (logic->pulse != TELIT_DIAG_PULSE_NONE ||
        !logic->off_threshold_set || sample_gap) {
        clear_status_low_tracking(logic);
    }
    if (observation.status_fresh) {
        if (!observation.status_valid ||
            observation.status_raw > logic->off_threshold_raw ||
            logic->pulse != TELIT_DIAG_PULSE_NONE ||
            !logic->off_threshold_set) {
            clear_status_low_tracking(logic);
        } else if (!logic->status_low_tracking) {
            logic->status_low_tracking = true;
            logic->status_low_stable = false;
            logic->status_low_since_ms = now_ms;
            logic->status_last_sample_ms = now_ms;
        } else if (deadline_reached(
                       now_ms, logic->status_low_since_ms +
                                   TELIT_DIAG_OFF_STABLE_MS)) {
            logic->status_low_stable = true;
            logic->status_last_sample_ms = now_ms;
        } else {
            logic->status_last_sample_ms = now_ms;
        }
    }
    if (logic->pulse != TELIT_DIAG_PULSE_NONE &&
        (!observation.power_good ||
         deadline_reached(now_ms, logic->pulse_deadline_ms))) {
        if (!observation.power_good &&
            logic->pulse == TELIT_DIAG_PULSE_BOOT) {
            logic->boot_attempted = false;
        }
        logic->pulse = TELIT_DIAG_PULSE_NONE;
        logic->pulse_width_ms = 0u;
        logic->pulse_deadline_ms = 0u;
    }
    if (!observation.power_good) {
        logic->boot_attempted = false;
        logic->dtr_sleep_permitted = false;
        logic->emergency_armed = false;
        logic->emergency_arm_deadline_ms = 0u;
    }
    if (logic->emergency_armed &&
        deadline_reached(now_ms, logic->emergency_arm_deadline_ms)) {
        logic->emergency_armed = false;
        logic->emergency_arm_deadline_ms = 0u;
    }
}

telit_diag_result_t telit_diag_request_rail_on(telit_diag_logic_t *logic,
                                                bool force_pwm) {
    if (logic == NULL || logic->rail_enabled) {
        return TELIT_DIAG_ERR_STATE;
    }
    clear_transient_controls(logic);
    logic->rail_enabled = true;
    logic->force_pwm = force_pwm;
    logic->dtr_sleep_permitted = false;
    logic->boot_attempted = false;
    clear_status_low_tracking(logic);
    return TELIT_DIAG_OK;
}

telit_diag_result_t telit_diag_request_rail_off(
    telit_diag_logic_t *logic, telit_diag_observation_t observation) {
    if (logic == NULL || !logic->rail_enabled ||
        logic->pulse != TELIT_DIAG_PULSE_NONE) {
        return TELIT_DIAG_ERR_STATE;
    }
    if (!logic->off_threshold_set) {
        return TELIT_DIAG_ERR_NEEDS_THRESHOLD;
    }
    if (!observation.status_valid) {
        return TELIT_DIAG_ERR_NEEDS_STATUS;
    }
    if (observation.status_raw > logic->off_threshold_raw) {
        return TELIT_DIAG_ERR_STATUS_HIGH;
    }
    if (!logic->status_low_stable) {
        return TELIT_DIAG_ERR_STATUS_UNSTABLE;
    }
    logic->rail_enabled = false;
    logic->force_pwm = false;
    logic->dtr_sleep_permitted = false;
    logic->boot_attempted = false;
    clear_status_low_tracking(logic);
    clear_transient_controls(logic);
    return TELIT_DIAG_OK;
}

telit_diag_result_t telit_diag_set_off_threshold(
    telit_diag_logic_t *logic, uint16_t raw) {
    if (logic == NULL) {
        return TELIT_DIAG_ERR_STATE;
    }
    if (raw > TELIT_DIAG_OFF_THRESHOLD_MAX_RAW) {
        return TELIT_DIAG_ERR_RANGE;
    }
    logic->off_threshold_set = true;
    logic->off_threshold_raw = raw;
    clear_status_low_tracking(logic);
    return TELIT_DIAG_OK;
}

telit_diag_result_t telit_diag_request_boot_pulse(
    telit_diag_logic_t *logic, uint32_t now_ms,
    telit_diag_observation_t observation, uint32_t width_ms) {
    if (width_ms < TELIT_DIAG_BOOT_PULSE_MIN_MS ||
        width_ms > TELIT_DIAG_BOOT_PULSE_MAX_MS) {
        return TELIT_DIAG_ERR_RANGE;
    }
    telit_diag_result_t result = start_pulse(
        logic, now_ms, observation, TELIT_DIAG_PULSE_BOOT, width_ms);
    if (result == TELIT_DIAG_OK) {
        logic->boot_attempted = true;
    }
    return result;
}

telit_diag_result_t telit_diag_request_hw_off_pulse(
    telit_diag_logic_t *logic, uint32_t now_ms,
    telit_diag_observation_t observation, uint32_t width_ms) {
    if (width_ms < TELIT_DIAG_HW_OFF_PULSE_MIN_MS ||
        width_ms > TELIT_DIAG_HW_OFF_PULSE_MAX_MS) {
        return TELIT_DIAG_ERR_RANGE;
    }
    if (logic == NULL || !logic->boot_attempted) {
        return TELIT_DIAG_ERR_STATE;
    }
    return start_pulse(
        logic, now_ms, observation, TELIT_DIAG_PULSE_HW_OFF, width_ms);
}

telit_diag_result_t telit_diag_request_emergency_arm(
    telit_diag_logic_t *logic, uint32_t now_ms,
    telit_diag_observation_t observation) {
    if (logic == NULL || !logic->rail_enabled || !logic->boot_attempted ||
        logic->pulse != TELIT_DIAG_PULSE_NONE) {
        return TELIT_DIAG_ERR_STATE;
    }
    if (!observation.power_good) {
        return TELIT_DIAG_ERR_NEEDS_PG;
    }
    logic->emergency_armed = true;
    logic->emergency_arm_deadline_ms =
        now_ms + TELIT_DIAG_EMERGENCY_ARM_MS;
    return TELIT_DIAG_OK;
}

telit_diag_result_t telit_diag_request_emergency_fire(
    telit_diag_logic_t *logic, uint32_t now_ms,
    telit_diag_observation_t observation) {
    if (logic == NULL || !logic->emergency_armed ||
        deadline_reached(now_ms, logic->emergency_arm_deadline_ms)) {
        return TELIT_DIAG_ERR_NOT_ARMED;
    }
    logic->emergency_armed = false;
    logic->emergency_arm_deadline_ms = 0u;
    return start_pulse(logic, now_ms, observation,
                       TELIT_DIAG_PULSE_EMERGENCY,
                       TELIT_DIAG_EMERGENCY_PULSE_MS);
}

telit_diag_result_t telit_diag_request_dtr(
    telit_diag_logic_t *logic, bool sleep_permitted,
    telit_diag_observation_t observation) {
    if (logic == NULL) {
        return TELIT_DIAG_ERR_STATE;
    }
    if (!sleep_permitted) {
        logic->dtr_sleep_permitted = false;
        return TELIT_DIAG_OK;
    }
    if (!logic->rail_enabled ||
        logic->pulse != TELIT_DIAG_PULSE_NONE) {
        return TELIT_DIAG_ERR_STATE;
    }
    if (!observation.power_good) {
        return TELIT_DIAG_ERR_NEEDS_PG;
    }
    logic->dtr_sleep_permitted = true;
    return TELIT_DIAG_OK;
}

void telit_diag_abort_pulse(telit_diag_logic_t *logic) {
    if (logic == NULL) {
        return;
    }
    if (logic->pulse == TELIT_DIAG_PULSE_BOOT) {
        logic->boot_attempted = false;
    }
    clear_transient_controls(logic);
}

bool telit_diag_uart_tx_allowed(const telit_diag_logic_t *logic,
                                telit_diag_observation_t observation) {
    return logic != NULL && logic->rail_enabled &&
           observation.power_good &&
           logic->pulse == TELIT_DIAG_PULSE_NONE;
}

bool telit_diag_on_off_asserted(const telit_diag_logic_t *logic) {
    return logic != NULL &&
           (logic->pulse == TELIT_DIAG_PULSE_BOOT ||
            logic->pulse == TELIT_DIAG_PULSE_HW_OFF);
}

bool telit_diag_emergency_asserted(const telit_diag_logic_t *logic) {
    return logic != NULL &&
           logic->pulse == TELIT_DIAG_PULSE_EMERGENCY;
}

const char *telit_diag_result_text(telit_diag_result_t result) {
    switch (result) {
    case TELIT_DIAG_OK:
        return "ok";
    case TELIT_DIAG_ERR_STATE:
        return "invalid state";
    case TELIT_DIAG_ERR_RANGE:
        return "out of safe range";
    case TELIT_DIAG_ERR_NEEDS_PG:
        return "PMIC power-good required";
    case TELIT_DIAG_ERR_NEEDS_STATUS:
        return "valid status ADC sample required";
    case TELIT_DIAG_ERR_NEEDS_THRESHOLD:
        return "off threshold required";
    case TELIT_DIAG_ERR_STATUS_HIGH:
        return "status above off threshold";
    case TELIT_DIAG_ERR_STATUS_UNSTABLE:
        return "status low interval not yet stable";
    case TELIT_DIAG_ERR_NOT_ARMED:
        return "emergency action not armed";
    default:
        return "unknown";
    }
}

const char *telit_diag_pulse_text(telit_diag_pulse_t pulse) {
    switch (pulse) {
    case TELIT_DIAG_PULSE_NONE:
        return "none";
    case TELIT_DIAG_PULSE_BOOT:
        return "boot";
    case TELIT_DIAG_PULSE_HW_OFF:
        return "hw-off";
    case TELIT_DIAG_PULSE_EMERGENCY:
        return "emergency";
    default:
        return "unknown";
    }
}
