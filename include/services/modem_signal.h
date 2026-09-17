#ifndef MODEM_SIGNAL_H
#define MODEM_SIGNAL_H

#include <stdint.h>

typedef enum {
    MODEM_SIGNAL_RAT_UNKNOWN = 0,
    MODEM_SIGNAL_RAT_GSM,
    MODEM_SIGNAL_RAT_WCDMA,
    MODEM_SIGNAL_RAT_LTE,
} modem_signal_rat_t;

enum {
    MODEM_SIGNAL_VALID_RSSI = 1u << 0,
    MODEM_SIGNAL_VALID_RSRP = 1u << 1,
    MODEM_SIGNAL_VALID_RSRQ = 1u << 2,
    MODEM_SIGNAL_VALID_SINR = 1u << 3,
    MODEM_SIGNAL_VALID_CHANNEL = 1u << 4,
    MODEM_SIGNAL_VALID_CELL_ID = 1u << 5,
    MODEM_SIGNAL_VALID_PLMN = 1u << 6,
};

/* Vendor-neutral serving-cell sample. Units are deliberately fixed point:
 * RSRP/RSSI are whole dBm, RSRQ is 0.5 dB, and SINR is 0.1 dB. */
typedef struct {
    uint32_t sequence;
    uint32_t updated_ms;
    uint32_t channel;
    uint32_t cell_id;
    int16_t rssi_dbm;
    int16_t rsrp_dbm;
    int16_t rsrq_db_x2;
    int16_t sinr_db_x10;
    uint16_t valid_fields;
    modem_signal_rat_t rat;
    /* Decimal strings preserve leading zeros and two/three-digit MNCs. */
    char mcc[4];
    char mnc[4];
} modem_signal_sample_t;

#endif
