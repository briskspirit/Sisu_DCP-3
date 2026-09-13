#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "ui/signal_bars.h"

static int s_failures;

static void check(bool condition, const char *message) {
    if (!condition) {
        fprintf(stderr, "FAIL: %s\n", message);
        s_failures++;
    }
}

static modem_signal_sample_t lte_sample(uint32_t sequence, int16_t rsrp_dbm,
                                        int16_t rsrq_db_x2,
                                        int16_t sinr_db_x10,
                                        bool sinr_valid) {
    modem_signal_sample_t sample;
    memset(&sample, 0, sizeof(sample));
    sample.sequence = sequence;
    sample.rat = MODEM_SIGNAL_RAT_LTE;
    sample.rsrp_dbm = rsrp_dbm;
    sample.rsrq_db_x2 = rsrq_db_x2;
    sample.sinr_db_x10 = sinr_db_x10;
    sample.channel = 5230u;
    sample.cell_id = UINT32_C(0xabcdef01);
    sample.valid_fields = MODEM_SIGNAL_VALID_RSRP |
        MODEM_SIGNAL_VALID_RSRQ | MODEM_SIGNAL_VALID_CHANNEL |
        MODEM_SIGNAL_VALID_CELL_ID;
    if (sinr_valid) sample.valid_fields |= MODEM_SIGNAL_VALID_SINR;
    return sample;
}

static uint8_t first_level(int16_t rsrp, int16_t rsrq_x2,
                           int16_t sinr_x10, bool sinr_valid) {
    signal_bars_filter_t filter;
    signal_bars_filter_init(&filter);
    modem_signal_sample_t sample =
        lte_sample(1u, rsrp, rsrq_x2, sinr_x10, sinr_valid);
    return signal_bars_filter_update(&filter, true, &sample, 4u);
}

static void test_thresholds_and_fusion(void) {
    check(first_level(-85, -24, 130, true) == 4u,
          "upper threshold endpoints map to four bars");
    check(first_level(-95, -28, 50, true) == 3u,
          "middle-high threshold endpoints map to three bars");
    check(first_level(-105, -34, 10, true) == 2u,
          "middle-low threshold endpoints map to two bars");
    check(first_level(-115, -38, -30, true) == 1u,
          "lowest registered threshold maps to one bar");
    check(first_level(-130, -50, -100, true) == 1u,
          "registered service reserves zero bars for no service");
    check(first_level(-107, -34, 12, true) == 1u,
          "proposed -107 dBm and +1.2 dB example fuses to one bar");

    check(first_level(-90, -36, 200, false) == 1u,
          "RSRQ is the quality fallback when SINR is unavailable");
    check(first_level(-90, -50, 200, true) == 3u,
          "valid SINR takes precedence over a poor RSRQ value");
}

static void test_registration_authority(void) {
    signal_bars_filter_t filter;
    signal_bars_filter_init(&filter);
    check(signal_bars_filter_update(&filter, true, NULL, 0u) == 1u,
          "registered phone with no fresh measurement shows one bar");
    modem_signal_sample_t sample = lte_sample(1u, -80, -20, 200, true);
    check(signal_bars_filter_update(&filter, true, &sample, 0u) == 4u,
          "first complete measurement publishes without a startup ramp");
    check(signal_bars_filter_update(&filter, false, &sample, 4u) == 0u,
          "registration loss drops immediately to zero");
    modem_signal_sample_t invalidated = { .sequence = 2u };
    check(signal_bars_filter_update(&filter, true, &invalidated, 4u) == 1u,
          "re-registration cannot reuse the pre-loss filter state");
}

static void test_ema_and_directional_hysteresis(void) {
    signal_bars_filter_t filter;
    signal_bars_filter_init(&filter);
    modem_signal_sample_t sample = lte_sample(1u, -80, -20, 200, true);
    check(signal_bars_filter_update(&filter, true, &sample, 0u) == 4u,
          "strong baseline starts at four bars");

    sample = lte_sample(2u, -130, -50, -100, true);
    check(signal_bars_filter_update(&filter, true, &sample, 0u) == 4u,
          "one lower filtered observation is held");
    sample.sequence = 3u;
    check(signal_bars_filter_update(&filter, true, &sample, 0u) == 2u,
          "second consecutive lower observation is accepted");

    signal_bars_filter_init(&filter);
    sample = lte_sample(1u, -110, -36, 0, true);
    check(signal_bars_filter_update(&filter, true, &sample, 0u) == 1u,
          "weak baseline starts at one bar");
    for (uint32_t sequence = 2u; sequence <= 5u; sequence++) {
        sample = lte_sample(sequence, -80, -20, 200, true);
        check(signal_bars_filter_update(&filter, true, &sample, 0u) == 1u,
              "first four consecutive increases are held");
    }
    sample = lte_sample(6u, -80, -20, 200, true);
    check(signal_bars_filter_update(&filter, true, &sample, 0u) == 3u,
          "fifth consecutive increase is accepted");
}

static void test_cell_change_resets_filter(void) {
    signal_bars_filter_t filter;
    signal_bars_filter_init(&filter);
    modem_signal_sample_t sample = lte_sample(1u, -80, -20, 200, true);
    check(signal_bars_filter_update(&filter, true, &sample, 0u) == 4u,
          "old cell starts strong");

    sample = lte_sample(2u, -110, -36, 0, true);
    sample.channel = 1025u;
    sample.cell_id = UINT32_C(0xabcdef02);
    check(signal_bars_filter_update(&filter, true, &sample, 0u) == 1u,
          "new EARFCN/cell identity publishes immediately");

    sample.rsrp_dbm = -80;
    sample.sinr_db_x10 = 200;
    check(signal_bars_filter_update(&filter, true, &sample, 0u) == 1u,
          "duplicate sequence cannot perturb the filter");
}

static void test_non_lte_uses_typed_rssi(void) {
    signal_bars_filter_t filter;
    signal_bars_filter_init(&filter);
    modem_signal_sample_t sample;
    memset(&sample, 0, sizeof(sample));
    sample.sequence = 1u;
    sample.rat = MODEM_SIGNAL_RAT_GSM;
    sample.valid_fields = MODEM_SIGNAL_VALID_RSSI;
    sample.rssi_dbm = -80;
    check(signal_bars_filter_update(&filter, true, &sample, 1u) == 3u,
          "non-LTE service uses typed RFSTS RSSI instead of stale CSQ");
}

int main(void) {
    test_thresholds_and_fusion();
    test_registration_authority();
    test_ema_and_directional_hysteresis();
    test_cell_change_resets_filter();
    test_non_lte_uses_typed_rssi();

    if (s_failures == 0) {
        puts("test_signal_bars: all passed");
        return 0;
    }
    fprintf(stderr, "test_signal_bars: %d failure(s)\n", s_failures);
    return 1;
}
