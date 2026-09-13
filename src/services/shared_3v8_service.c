#include "services/shared_3v8_service.h"

#include "hal/board.h"
#include "services/log.h"
#include "pico/stdlib.h"

#define SHARED_3V8_VALID_OWNERS \
    ((uint8_t)SHARED_3V8_OWNER_MODEM | (uint8_t)SHARED_3V8_OWNER_AUDIO | \
     (uint8_t)SHARED_3V8_OWNER_DIAGNOSTIC)

static uint8_t s_owner_mask;
static uint32_t s_transition_count;

static bool valid_single_owner(shared_3v8_owner_t owner) {
    uint8_t bit = (uint8_t)owner;
    return bit != 0u && (bit & (uint8_t)~SHARED_3V8_VALID_OWNERS) == 0u &&
           (bit & (uint8_t)(bit - 1u)) == 0u;
}

void shared_3v8_service_init(void) {
    s_owner_mask = 0u;
    s_transition_count = 0u;
    board_3v8_rail_set_force_pwm(false);
    board_3v8_rail_set_enabled(false);
}

void shared_3v8_service_set_required(shared_3v8_owner_t owner, bool required) {
    if (!valid_single_owner(owner)) {
        LOGE("3v8", "invalid owner %u", (unsigned)owner);
        return;
    }

    uint8_t previous = s_owner_mask;
    if (required) {
        s_owner_mask |= (uint8_t)owner;
    } else {
        s_owner_mask &= (uint8_t)~(uint8_t)owner;
    }
    if (s_owner_mask == previous) {
        return;
    }

    if (previous == 0u) {
        board_3v8_rail_set_enabled(true);
        s_transition_count++;
    } else if (s_owner_mask == 0u) {
        board_3v8_rail_set_enabled(false);
        s_transition_count++;
    }
}

bool shared_3v8_service_wait_power_good(uint32_t timeout_us) {
    if (s_owner_mask == 0u || !board_3v8_rail_enabled()) {
        return false;
    }
    absolute_time_t deadline = make_timeout_time_us(timeout_us);
    do {
        if (board_3v8_rail_power_good()) {
            return true;
        }
        tight_loop_contents();
    } while (!time_reached(deadline));
    return board_3v8_rail_power_good();
}

uint8_t shared_3v8_service_owner_mask(void) {
    return s_owner_mask;
}

bool shared_3v8_service_enabled(void) {
    return board_3v8_rail_enabled();
}

uint32_t shared_3v8_service_transition_count(void) {
    return s_transition_count;
}

void shared_3v8_service_force_off(void) {
    bool was_enabled = board_3v8_rail_enabled();
    s_owner_mask = 0u;
    board_3v8_rail_set_enabled(false);
    if (was_enabled) {
        s_transition_count++;
    }
}
