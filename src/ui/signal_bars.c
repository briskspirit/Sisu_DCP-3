#include "ui/signal_bars.h"

#include <string.h>

#define SIGNAL_Q8_ONE 256
#define SIGNAL_LEVEL_INVALID 0xffu
#define SIGNAL_DECREASE_SAMPLES 2u
#define SIGNAL_INCREASE_SAMPLES 5u

static uint8_t metric_level_q8(int32_t value_q8,
                               const int16_t thresholds[4]) {
    for (uint8_t i = 0u; i < 4u; i++) {
        if (value_q8 < (int32_t)thresholds[i] * SIGNAL_Q8_ONE) {
            return i;
        }
    }
    return 4u;
}

static uint8_t min_valid(uint8_t a, uint8_t b) {
    if (a == SIGNAL_LEVEL_INVALID) return b;
    if (b == SIGNAL_LEVEL_INVALID) return a;
    return a < b ? a : b;
}

static uint8_t registered_rssi_bars(int16_t rssi_dbm) {
    /* Preserve the old CSQ visual breakpoints using 3GPP's
     * dBm = -113 + 2 * CSQ conversion, but consume the typed RFSTS value. */
    if (rssi_dbm >= -69) return 4u;
    if (rssi_dbm >= -81) return 3u;
    if (rssi_dbm >= -93) return 2u;
    return 1u;
}

static int32_t ema_quarter(int32_t filtered_q8, int16_t sample) {
    return filtered_q8 +
        (((int32_t)sample * SIGNAL_Q8_ONE - filtered_q8) / 4);
}

static void reset_measurements(signal_bars_filter_t *filter) {
    filter->filtered_fields = 0u;
    filter->identity_fields = 0u;
    filter->metrics_initialized = false;
    filter->trend_count = 0u;
    filter->trend_bars = 0u;
}

void signal_bars_filter_init(signal_bars_filter_t *filter) {
    if (filter != NULL) {
        memset(filter, 0, sizeof(*filter));
    }
}

static bool identity_changed(const signal_bars_filter_t *filter,
                             const modem_signal_sample_t *sample) {
    if (!filter->metrics_initialized || filter->rat != sample->rat) {
        return filter->metrics_initialized;
    }
    if ((filter->identity_fields & MODEM_SIGNAL_VALID_CHANNEL) != 0u &&
        (sample->valid_fields & MODEM_SIGNAL_VALID_CHANNEL) != 0u &&
        filter->channel != sample->channel) {
        return true;
    }
    return (filter->identity_fields & MODEM_SIGNAL_VALID_CELL_ID) != 0u &&
           (sample->valid_fields & MODEM_SIGNAL_VALID_CELL_ID) != 0u &&
           filter->cell_id != sample->cell_id;
}

static void remember_identity(signal_bars_filter_t *filter,
                              const modem_signal_sample_t *sample) {
    filter->rat = sample->rat;
    filter->identity_fields = sample->valid_fields &
        (MODEM_SIGNAL_VALID_CHANNEL | MODEM_SIGNAL_VALID_CELL_ID);
    if ((sample->valid_fields & MODEM_SIGNAL_VALID_CHANNEL) != 0u) {
        filter->channel = sample->channel;
    }
    if ((sample->valid_fields & MODEM_SIGNAL_VALID_CELL_ID) != 0u) {
        filter->cell_id = sample->cell_id;
    }
}

static void filter_metric(int32_t *filtered_q8, uint16_t bit,
                          uint16_t sample_fields, int16_t value,
                          signal_bars_filter_t *filter, bool reset) {
    if ((sample_fields & bit) == 0u) {
        /* A later reappearance is a fresh metric, not an EMA continuation from
         * an arbitrarily old value. This gives "SINR stale" the same behavior
         * as unavailable: use RSRQ now, re-seed SINR when it returns. */
        filter->filtered_fields &= (uint16_t)~bit;
        return;
    }
    if (reset || (filter->filtered_fields & bit) == 0u) {
        *filtered_q8 = (int32_t)value * SIGNAL_Q8_ONE;
    } else {
        *filtered_q8 = ema_quarter(*filtered_q8, value);
    }
    filter->filtered_fields |= bit;
}

static uint8_t fused_lte_level(const signal_bars_filter_t *filter,
                               uint16_t sample_fields) {
    static const int16_t rsrp_thresholds[4] = {-115, -105, -95, -85};
    /* RSRQ is x2 and SINR is x10. */
    static const int16_t rsrq_thresholds[4] = {-38, -34, -28, -24};
    static const int16_t sinr_thresholds[4] = {-30, 10, 50, 130};

    uint8_t power = SIGNAL_LEVEL_INVALID;
    uint8_t quality = SIGNAL_LEVEL_INVALID;
    if ((sample_fields & MODEM_SIGNAL_VALID_RSRP) != 0u) {
        power = metric_level_q8(filter->rsrp_q8, rsrp_thresholds);
    }
    if ((sample_fields & MODEM_SIGNAL_VALID_SINR) != 0u) {
        quality = metric_level_q8(filter->sinr_q8, sinr_thresholds);
    } else if ((sample_fields & MODEM_SIGNAL_VALID_RSRQ) != 0u) {
        quality = metric_level_q8(filter->rsrq_q8, rsrq_thresholds);
    }
    uint8_t level = min_valid(power, quality);
    if (level == SIGNAL_LEVEL_INVALID || level == 0u) {
        return 1u;
    }
    return level;
}

static uint8_t apply_trend(signal_bars_filter_t *filter, uint8_t candidate) {
    if (candidate == filter->bars) {
        filter->trend_count = 0u;
        filter->trend_bars = candidate;
        return filter->bars;
    }

    bool increasing = candidate > filter->bars;
    bool prior_increasing = filter->trend_bars > filter->bars;
    bool prior_decreasing = filter->trend_bars < filter->bars;
    if (filter->trend_count == 0u ||
        (increasing && !prior_increasing) ||
        (!increasing && !prior_decreasing)) {
        filter->trend_count = 1u;
    } else if (filter->trend_count != UINT8_MAX) {
        filter->trend_count++;
    }
    filter->trend_bars = candidate;

    uint8_t required = increasing ? SIGNAL_INCREASE_SAMPLES
                                  : SIGNAL_DECREASE_SAMPLES;
    if (filter->trend_count >= required) {
        filter->bars = candidate;
        filter->trend_count = 0u;
    }
    return filter->bars;
}

uint8_t signal_bars_filter_update(signal_bars_filter_t *filter,
                                  bool registered,
                                  const modem_signal_sample_t *sample,
                                  uint8_t fallback_bars) {
    if (filter == NULL) {
        return registered ? (fallback_bars == 0u ? 1u : fallback_bars) : 0u;
    }
    if (!registered) {
        signal_bars_filter_init(filter);
        return 0u;
    }

    if (sample == NULL || sample->sequence == 0u) {
        filter->bars = filter->bars == 0u ? 1u : filter->bars;
        return filter->bars;
    }
    if (filter->sequence_seen && sample->sequence == filter->last_sequence) {
        return filter->bars == 0u ? 1u : filter->bars;
    }
    filter->sequence_seen = true;
    filter->last_sequence = sample->sequence;

    if (sample->rat == MODEM_SIGNAL_RAT_UNKNOWN) {
        reset_measurements(filter);
        filter->rat = MODEM_SIGNAL_RAT_UNKNOWN;
        filter->bars = 1u;
        return filter->bars;
    }
    if (sample->rat != MODEM_SIGNAL_RAT_LTE) {
        reset_measurements(filter);
        filter->rat = sample->rat;
        filter->bars =
            (sample->valid_fields & MODEM_SIGNAL_VALID_RSSI) != 0u
                ? registered_rssi_bars(sample->rssi_dbm)
                : (fallback_bars == 0u ? 1u : fallback_bars);
        return filter->bars;
    }

    bool reset = identity_changed(filter, sample);
    if (reset) {
        reset_measurements(filter);
    }
    remember_identity(filter, sample);
    filter_metric(&filter->rsrp_q8, MODEM_SIGNAL_VALID_RSRP,
                  sample->valid_fields, sample->rsrp_dbm, filter, reset);
    filter_metric(&filter->rsrq_q8, MODEM_SIGNAL_VALID_RSRQ,
                  sample->valid_fields, sample->rsrq_db_x2, filter, reset);
    filter_metric(&filter->sinr_q8, MODEM_SIGNAL_VALID_SINR,
                  sample->valid_fields, sample->sinr_db_x10, filter, reset);

    uint16_t usable = sample->valid_fields & filter->filtered_fields;
    uint8_t candidate = fused_lte_level(filter, usable);
    if (!filter->metrics_initialized || reset) {
        filter->metrics_initialized = true;
        filter->bars = candidate;
        filter->trend_count = 0u;
        return filter->bars;
    }
    return apply_trend(filter, candidate);
}
