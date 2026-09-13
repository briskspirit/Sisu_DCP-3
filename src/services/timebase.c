#include "services/timebase.h"

#include "hal/board.h"
#include "pico/time.h"

static uint64_t s_dormant_offset_ms;

static uint64_t raw_time_ms64(void) {
    return to_us_since_boot(get_absolute_time()) / 1000u;
}

uint32_t time_ms(void) {
    return (uint32_t)time_ms64();
}

uint64_t time_ms64(void) {
    return raw_time_ms64() + s_dormant_offset_ms;
}

uint32_t time_ticks8(void) {
    return time_ms() / SYSTEM_TICK_MS;
}

int32_t time_diff_ms(uint32_t a, uint32_t b) {
    return (int32_t)(a - b);
}

void timebase_rebase_ms(uint64_t target_ms) {
    uint64_t raw_ms = raw_time_ms64();
    uint64_t current_ms = raw_ms + s_dormant_offset_ms;
    if (target_ms < current_ms || target_ms < raw_ms) {
        return; /* wall time is monotonic even if an AON sample rounded down */
    }
    s_dormant_offset_ms = target_ms - raw_ms;
}

uint64_t timebase_offset_ms(void) {
    return s_dormant_offset_ms;
}
