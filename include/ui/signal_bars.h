#ifndef SIGNAL_BARS_H
#define SIGNAL_BARS_H

#include <stdbool.h>
#include <stdint.h>

#include "services/modem_signal.h"

typedef struct {
    uint32_t last_sequence;
    uint32_t channel;
    uint32_t cell_id;
    int32_t rsrp_q8;
    int32_t rsrq_q8;
    int32_t sinr_q8;
    uint16_t filtered_fields;
    uint16_t identity_fields;
    modem_signal_rat_t rat;
    uint8_t bars;
    uint8_t trend_bars;
    uint8_t trend_count;
    bool sequence_seen;
    bool metrics_initialized;
} signal_bars_filter_t;

void signal_bars_filter_init(signal_bars_filter_t *filter);

/* LTE uses fused RSRP and SINR/RSRQ. Other RATs retain the existing coarse
 * signal source through fallback_bars. Zero bars is reserved for no service. */
uint8_t signal_bars_filter_update(signal_bars_filter_t *filter,
                                  bool registered,
                                  const modem_signal_sample_t *sample,
                                  uint8_t fallback_bars);

#endif
