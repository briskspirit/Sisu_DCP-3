#ifndef MODEM_DIAG_H
#define MODEM_DIAG_H

#include <stdbool.h>
#include <stdint.h>

#include "services/call_types.h"
#include "services/message_waiting_types.h"

#define MODEM_DIAG_TUNER_ROWS_MAX 4u
#define MODEM_DIAG_CALL_LEGS_MAX MODEM_CALL_ID_MAX
#define MODEM_DIAG_CALL_TXNS_MAX 12u
#define MODEM_DIAG_TERMINALS_MAX MODEM_CALL_ID_MAX
/* 3GPP TS 23.003 permits an encoded APN up to 100 octets. Telit's IPv6
 * ip&subnet form can occupy 127 printable dot-decimal characters. */
#define MODEM_DIAG_PACKET_APN_MAX 100u
#define MODEM_DIAG_PACKET_ADDRESS_MAX 127u
#define MODEM_DIAG_PACKET_APN_PREVIEW_MAX 24u
#define MODEM_DIAG_PACKET_ADDRESS_PREVIEW_MAX 40u

typedef enum {
    MODEM_DIAG_STATE_UNKNOWN = 0,
    MODEM_DIAG_STATE_PENDING,
    MODEM_DIAG_STATE_FRESH,
    MODEM_DIAG_STATE_STALE,
    MODEM_DIAG_STATE_UNSUPPORTED,
    MODEM_DIAG_STATE_ERROR,
} modem_diag_state_t;

typedef enum {
    MODEM_DIAG_ERROR_NONE = 0,
    MODEM_DIAG_ERROR_REJECTED,
    MODEM_DIAG_ERROR_TIMEOUT,
    MODEM_DIAG_ERROR_MALFORMED,
    MODEM_DIAG_ERROR_CANCELLED,
    MODEM_DIAG_ERROR_UNAVAILABLE,
} modem_diag_error_t;

typedef enum {
    MODEM_DIAG_GROUP_NONE = 0,
    MODEM_DIAG_GROUP_SERVING,
    MODEM_DIAG_GROUP_REGISTRATION,
    MODEM_DIAG_GROUP_RADIO_POLICY,
    MODEM_DIAG_GROUP_TUNER,
    MODEM_DIAG_GROUP_PACKET,
    MODEM_DIAG_GROUP_SIM,
    MODEM_DIAG_GROUP_VOICE,
    MODEM_DIAG_GROUP_TEMPERATURE,
    MODEM_DIAG_GROUP_IDENTITY,
    MODEM_DIAG_GROUP_DVI,
    MODEM_DIAG_GROUP_CALL_CAUSE,
    MODEM_DIAG_GROUP_SMS_CONFIG,
    MODEM_DIAG_GROUP_STORAGE_CAPS,
    MODEM_DIAG_GROUP_COUNT,
} modem_diag_group_t;

typedef enum {
    MODEM_DIAG_RAT_UNKNOWN = 0,
    MODEM_DIAG_RAT_GSM,
    MODEM_DIAG_RAT_WCDMA,
    MODEM_DIAG_RAT_LTE,
} modem_diag_rat_t;

typedef struct {
    modem_diag_state_t state;
    modem_diag_error_t last_error;
    uint32_t last_attempt_ms;
    uint32_t last_success_ms;
    uint32_t sequence;
    uint8_t consecutive_failures; /* Rejected, timed-out, or malformed groups. */
    uint64_t present_fields;
} modem_diag_group_meta_t;

enum {
    MODEM_DIAG_SERVING_RAT = 1ull << 0,
    MODEM_DIAG_SERVING_PLMN = 1ull << 1,
    MODEM_DIAG_SERVING_CHANNEL = 1ull << 2,
    MODEM_DIAG_SERVING_BAND = 1ull << 3,
    MODEM_DIAG_SERVING_RSSI = 1ull << 4,
    MODEM_DIAG_SERVING_RSRP = 1ull << 5,
    MODEM_DIAG_SERVING_RSRQ = 1ull << 6,
    MODEM_DIAG_SERVING_SINR = 1ull << 7,
    MODEM_DIAG_SERVING_TX_POWER = 1ull << 8,
    MODEM_DIAG_SERVING_AREA = 1ull << 9,
    MODEM_DIAG_SERVING_CELL = 1ull << 10,
    MODEM_DIAG_SERVING_PCI = 1ull << 11,
    MODEM_DIAG_SERVING_OPERATOR = 1ull << 12,
    MODEM_DIAG_SERVING_DRX = 1ull << 13,
    MODEM_DIAG_SERVING_MM = 1ull << 14,
    MODEM_DIAG_SERVING_RRC = 1ull << 15,
    MODEM_DIAG_SERVING_DOMAIN = 1ull << 16,
};

typedef struct {
    modem_diag_rat_t rat;
    char mcc[4];
    char mnc[4];
    char operator_name[17];
    uint32_t channel;
    uint16_t band;
    int16_t rssi_dbm;
    int16_t rsrp_dbm;
    int16_t rsrq_db_x2;
    int16_t sinr_db_x10;
    int16_t tx_power_dbm_x10;
    char area_code[5];
    char cell_id[9];
    uint16_t pci;
    uint16_t drx_ms;
    uint8_t mm_state;
    uint8_t rrc_state;
    uint8_t service_domain;
    uint8_t inferred_rf_state;
    bool inferred_rf_tuned;
} modem_diag_serving_t;

enum {
    MODEM_DIAG_REG_CS = 1ull << 0,
    MODEM_DIAG_REG_PS = 1ull << 1,
    MODEM_DIAG_REG_EPS = 1ull << 2,
    MODEM_DIAG_REG_IMS = 1ull << 3,
};

typedef struct {
    uint8_t cs_stat;
    uint8_t ps_stat;
    uint8_t eps_stat;
    uint8_t ims_stat;
    uint8_t cs_act;
    uint8_t ps_act;
    uint8_t eps_act;
    char cs_area[5];
    char ps_area[5];
    char eps_area[5];
    char cs_cell[9];
    char ps_cell[9];
    char eps_cell[9];
} modem_diag_registration_t;

enum {
    MODEM_DIAG_POLICY_CFUN = 1ull << 0,
    MODEM_DIAG_POLICY_COPS = 1ull << 1,
    MODEM_DIAG_POLICY_ENS = 1ull << 2,
    MODEM_DIAG_POLICY_FWSWITCH = 1ull << 3,
    MODEM_DIAG_POLICY_WS46 = 1ull << 4,
    MODEM_DIAG_POLICY_SELBNDMODE = 1ull << 5,
    MODEM_DIAG_POLICY_BND = 1ull << 6,
    MODEM_DIAG_POLICY_BNDRAM = 1ull << 7,
    MODEM_DIAG_POLICY_SCAN_CONFIG = 1ull << 8,
    MODEM_DIAG_POLICY_SCAN_REMAINING = 1ull << 9,
    MODEM_DIAG_POLICY_FWAUTOSIM = 1ull << 10,
};

typedef struct {
    uint8_t cfun;
    uint8_t cops_mode;
    uint8_t cops_format;
    uint8_t cops_act;
    char cops_operator[17];
    uint8_t ens;
    uint16_t firmware_image;
    uint8_t firmware_storage;
    uint8_t firmware_restore;
    uint8_t firmware_auto_sim;
    uint8_t ws46;
    uint8_t select_band_mode;
    uint8_t bnd_gsm;
    uint16_t bnd_wcdma;
    char bnd_lte[17];
    uint8_t bndram_gsm;
    uint16_t bndram_wcdma;
    char bndram_lte[17];
    uint16_t scan_timer_s;
    uint16_t scan_remaining_s;
} modem_diag_radio_policy_t;

typedef struct {
    uint64_t band_mask;
    uint8_t ctrl1;
    uint8_t ctrl2;
} modem_diag_tuner_row_t;

enum {
    MODEM_DIAG_TUNER_ENABLED = 1ull << 0,
    MODEM_DIAG_TUNER_SUPPORTED_MASK = 1ull << 1,
    MODEM_DIAG_TUNER_TABLE = 1ull << 2,
};

typedef struct {
    bool enabled;
    bool table_exact;
    bool table_complete;
    uint64_t supported_mask;
    uint8_t row_count;
    modem_diag_tuner_row_t rows[MODEM_DIAG_TUNER_ROWS_MAX];
} modem_diag_tuner_t;

enum {
    MODEM_DIAG_PACKET_ATTACH = 1ull << 0,
    MODEM_DIAG_PACKET_CONTEXTS = 1ull << 1,
    MODEM_DIAG_PACKET_ADDRESS = 1ull << 2,
    MODEM_DIAG_PACKET_APN = 1ull << 3,
    MODEM_DIAG_PACKET_ADDRESS_TRUNCATED = 1ull << 4,
    MODEM_DIAG_PACKET_APN_TRUNCATED = 1ull << 5,
};

typedef struct {
    uint8_t attached;
    uint8_t context_count;
    uint8_t active_context_count;
    uint8_t first_cid;
    uint8_t first_status;
    /* NetMonitor previews remain bounded so each atomic snapshot fits the
     * core-0 copy budget. The present-field flags disclose truncation. */
    char apn[MODEM_DIAG_PACKET_APN_PREVIEW_MAX + 1u];
    char address[MODEM_DIAG_PACKET_ADDRESS_PREVIEW_MAX + 1u];
} modem_diag_packet_t;

enum {
    MODEM_DIAG_SIM_QSS = 1ull << 0,
    MODEM_DIAG_SIM_CPIN = 1ull << 1,
};

typedef struct {
    uint8_t qss_mode;
    uint8_t qss_status;
    char cpin[17];
} modem_diag_sim_t;

enum {
    MODEM_DIAG_VOICE_IMS_REG = 1ull << 0,
    MODEM_DIAG_VOICE_DOMAIN = 1ull << 1,
};

typedef struct {
    uint8_t ims_registration;
    uint8_t service_domain;
} modem_diag_voice_t;

typedef struct {
    int8_t level;
    int16_t celsius;
} modem_diag_temperature_t;

enum {
    MODEM_DIAG_ID_MODEL = 1ull << 0,
    MODEM_DIAG_ID_FIRMWARE = 1ull << 1,
    MODEM_DIAG_ID_IMEI = 1ull << 2,
};

typedef struct {
    char model[25];
    char firmware[33];
    char imei[16];
} modem_diag_identity_t;

enum {
    MODEM_DIAG_DVI_BASIC = 1ull << 0,
    MODEM_DIAG_DVI_EXT = 1ull << 1,
};

typedef struct {
    uint8_t enabled;
    uint8_t mode;
    uint8_t clock;
    uint8_t config;
    uint8_t sample_rate;
    uint8_t sample_width;
    uint8_t audio_mode;
    uint8_t edge;
} modem_diag_dvi_t;

typedef struct {
    char ceer[49];
} modem_diag_call_cause_t;

enum {
    MODEM_DIAG_SMS_CSMS = 1ull << 0,
    MODEM_DIAG_SMS_CNMI = 1ull << 1,
    MODEM_DIAG_SMS_CSMP = 1ull << 2,
    MODEM_DIAG_SMS_CSCA = 1ull << 3,
    MODEM_DIAG_SMS_CSDH = 1ull << 4,
    MODEM_DIAG_SMS_IMS = 1ull << 5,
    MODEM_DIAG_SMS_MWI = 1ull << 6,
};

typedef struct {
    uint8_t csms_service;
    uint8_t csms_mt;
    uint8_t csms_mo;
    uint8_t csms_bm;
    uint8_t cnmi_mode;
    uint8_t cnmi_mt;
    uint8_t cnmi_bm;
    uint8_t cnmi_ds;
    uint8_t cnmi_bfr;
    uint8_t csmp_fo;
    uint8_t csmp_vp;
    uint8_t csmp_pid;
    uint8_t csmp_dcs;
    char service_center[MODEM_PHONE_MAX + 1u];
    uint8_t service_center_type;
    uint8_t csdh;
    uint8_t ims_mode;
    uint8_t mwi_enabled;
    modem_message_waiting_status_t message_waiting;
} modem_diag_sms_t;

enum {
    MODEM_DIAG_STORAGE_SMS = 1ull << 0,
    MODEM_DIAG_STORAGE_PHONEBOOK = 1ull << 1,
};

typedef struct {
    char sms_read_store[3];
    char sms_write_store[3];
    char sms_receive_store[3];
    uint16_t sms_used;
    uint16_t sms_total;
    char phonebook_store[3];
    uint16_t phonebook_used;
    uint16_t phonebook_total;
} modem_diag_storage_t;

typedef struct {
    uint32_t generation;
    modem_diag_group_t selected_group;
    modem_diag_group_t active_group;
    uint8_t active_query;
    uint8_t active_query_count;
    bool pending;
    uint32_t admissions;
    uint32_t coalesced;
    uint32_t cancelled;
    uint32_t completed;
    uint32_t command_failures;
    uint32_t command_timeouts;
    uint32_t malformed_lines;
    uint32_t max_command_latency_ms;
    bool background_polling_enabled;
    uint8_t normal_queue_depth;
    uint8_t normal_queue_high_water;
    uint32_t normal_queue_admission_failures;
    uint32_t normal_queue_evictions;
} modem_diag_scheduler_t;

typedef struct {
    uint32_t rx_bytes;
    uint32_t rx_overruns;
    uint32_t rx_dropped;
    uint32_t rx_line_errors;
    uint32_t tx_stall_drops;
    bool dtr_sleep_permitted;
    bool dtr_wake_pending;
    bool ri_release_pending;
    bool cts_asserted;
    bool ri_asserted;
    uint32_t sleep_entries;
    uint32_t wake_attempts;
    uint32_t wake_timeouts;
    uint32_t ri_release_timeouts;
    uint32_t last_wake_latency_ms;
    uint32_t max_wake_latency_ms;
    /* Tick-accounted residency while the modem service is READY. Counters
     * intentionally wrap at uint32_t; unsigned snapshot deltas remain valid
     * for measurement windows shorter than 49 days. */
    uint32_t ready_ms;
    uint32_t sleep_requested_ms;
    uint32_t sleep_confirmed_ms;
} modem_diag_transport_t;

typedef struct {
    uint8_t state;
    uint8_t active_kind;
    uint8_t operation;
    uint8_t init_index;
    uint8_t init_retries;
    uint8_t provision_index;
    uint8_t provision_phase;
    uint8_t provision_retries;
    bool provisioning_verified;
    uint16_t provisioning_schema;
    bool audio_init_ok;
    bool sms_init_ok;
    uint32_t active_since_ms;
    uint32_t power_on_requests;
    uint32_t power_on_starts;
    uint32_t ready_entries;
    uint32_t power_off_requests;
    uint32_t shutdown_completions;
    uint32_t graceful_shutdown_pulses;
    uint32_t emergency_shutdown_pulses;
    uint32_t terminal_shutdown_failures;
    uint32_t automatic_recoveries;
    uint32_t controlled_restarts;
    uint32_t power_failures;
    uint32_t last_transition_ms;
    uint8_t last_recovery_reason;
    uint8_t shutdown_stage;
    bool shutdown_terminal_fault;
    char last_command[48];
    char last_line[48];
} modem_diag_runtime_t;

typedef enum {
    MODEM_DIAG_RECOVERY_NONE = 0,
    MODEM_DIAG_RECOVERY_STARTUP_STATUS,
    MODEM_DIAG_RECOVERY_STARTUP_AT,
    MODEM_DIAG_RECOVERY_RUNTIME_WEDGE,
    MODEM_DIAG_RECOVERY_PROVISION_REBOOT,
    MODEM_DIAG_RECOVERY_POWER_FAULT,
} modem_diag_recovery_reason_t;

typedef struct {
    uint8_t id;
    uint16_t generation;
    uint8_t direction;
    uint8_t state;
    uint8_t role;
} modem_diag_call_leg_t;

typedef struct {
    uint32_t token;
    uint8_t kind;
    uint8_t state;
    uint8_t target_id;
    uint16_t target_generation;
} modem_diag_call_txn_t;

typedef struct {
    modem_call_state_t projected_state;
    modem_call_result_t last_result;
    modem_call_result_t second_result;
    uint8_t active_call_id;
    bool ringing;
    bool waiting;
    bool on_hold;
    bool second_held;
    bool wants_clcc;
    uint8_t leg_count;
    modem_diag_call_leg_t legs[MODEM_DIAG_CALL_LEGS_MAX];
    uint8_t txn_count;
    modem_diag_call_txn_t txns[MODEM_DIAG_CALL_TXNS_MAX];
    uint8_t terminal_count;
    uint32_t terminal_sequence;
} modem_diag_calls_t;

typedef struct {
    uint32_t updated_ms;
    bool backend_available;
    bool at_ready;
    bool sim_checked;
    bool sim_present;
    bool sim_ready;
    bool network_registered;
    uint32_t urc_count;
    uint32_t command_errors;
    uint32_t sms_received_count;
    uint32_t sms_sent_count;
    uint32_t sms_storage_full_events;
    modem_diag_group_meta_t group[MODEM_DIAG_GROUP_COUNT];
    modem_diag_serving_t serving;
    modem_diag_registration_t registration;
    modem_diag_radio_policy_t radio_policy;
    modem_diag_tuner_t tuner;
    modem_diag_packet_t packet;
    modem_diag_sim_t sim;
    modem_diag_voice_t voice;
    modem_diag_temperature_t temperature;
    modem_diag_identity_t identity;
    modem_diag_dvi_t dvi;
    modem_diag_call_cause_t call_cause;
    modem_diag_sms_t sms;
    modem_diag_storage_t storage;
    modem_diag_scheduler_t scheduler;
    modem_diag_transport_t transport;
    modem_diag_runtime_t runtime;
    modem_diag_calls_t calls;
} modem_diag_snapshot_t;

typedef enum {
    MODEM_DIAG_LINE_IGNORE = 0,
    MODEM_DIAG_LINE_ACCEPT,
    MODEM_DIAG_LINE_INVALID,
} modem_diag_line_result_t;

/* A backend-provided diagnostic query. The generic engine owns scheduling,
 * generation cancellation, atomic publication, and error classification; a
 * query owns only its command grammar and writes into a query-local shadow. */
typedef struct {
    modem_diag_group_t group;
    const char *cmd;
    uint32_t timeout_ms;
    const char *prefix;
    uint8_t minimum_lines;
    bool optional;
    /* Some optional detail commands are informational only: malformed rows
     * are counted but must not discard unrelated mandatory fields. */
    bool isolate_malformed;
    modem_diag_line_result_t (*parse)(const char *line,
                                      modem_diag_snapshot_t *shadow);
} modem_diag_query_t;

typedef bool (*modem_diag_group_finish_fn)(modem_diag_group_t group,
                                           modem_diag_snapshot_t *shadow);

#endif
