#ifndef TIMEBASE_H
#define TIMEBASE_H

#include <stdint.h>

uint32_t time_ms(void);
uint64_t time_ms64(void);
uint32_t time_ticks8(void);
int32_t time_diff_ms(uint32_t a, uint32_t b);
/* TIMER0 stops in clock-DORMANT. Rebase wall time after clocks return and while
 * core1 plus core0 IRQs are still parked, so no observer can see a torn offset. */
void timebase_rebase_ms(uint64_t target_ms);
uint64_t timebase_offset_ms(void);

#endif
