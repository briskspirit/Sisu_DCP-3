#ifndef MODEM_VENDOR_TELIT_INTERNAL_H
#define MODEM_VENDOR_TELIT_INTERNAL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "services/modem_vendor.h"

#define TELIT_FIELD_CAP 48u
#define TELIT_RFSTS_FIELD_COUNT 16u
#define TELIT_SERVINFO_FIELD_COUNT 9u
#define TELIT_TUNE_COMMAND_DOMAIN_MASK ((UINT64_C(1) << 35u) - 1u)
#define TELIT_DIAG_QUERY_COUNT 43u
#define TELIT_INIT_STEP_COUNT 22u
#define TELIT_PROVISION_STEP_COUNT 28u

typedef struct {
    const char *text;
    uint8_t length;
} telit_csv_view_t;

typedef struct {
    uint8_t mode;
    uint8_t status;
} telit_qss_state_t;

typedef struct {
    uint8_t enabled;
    modem_message_waiting_category_t category;
    bool active;
    uint16_t count;
} telit_mwi_state_t;

typedef struct {
    int8_t level;
    int16_t celsius;
} telit_temperature_t;

typedef struct {
    uint16_t image;
    uint8_t storage;
    uint8_t restore;
} telit_fwswitch_t;

typedef enum {
    TELIT_TUNE_RF1 = 0,
    TELIT_TUNE_RF2,
    TELIT_TUNE_RF3,
    TELIT_TUNE_RF4,
    TELIT_TUNE_RF_COUNT,
} telit_tune_rf_t;

extern const modem_init_step_t TELIT_INIT_STEPS[];
extern const modem_provision_step_t TELIT_PROVISION_STEPS[];

bool telit_starts_with(const char *text, const char *prefix);
bool telit_csv_view_split(const char *body, telit_csv_view_t *fields,
                          size_t max_fields, size_t *count_out);
bool telit_view_split_prefixed(const char *line, const char *prefix,
                               telit_csv_view_t *fields, size_t max_fields,
                               size_t *count_out);
bool telit_view_find_labeled(const char *line, const char *label,
                             telit_csv_view_t *field_out);
bool telit_view_parse_u32(telit_csv_view_t field, uint32_t max_value,
                          uint32_t *value_out);
bool telit_view_parse_hex_u64(telit_csv_view_t field, uint64_t max_value,
                              uint64_t *value_out);
bool telit_view_parse_i32(telit_csv_view_t field, int32_t min_value,
                          int32_t max_value, int32_t *value_out);
bool telit_view_parse_decimal_x2(telit_csv_view_t field, int16_t min_x2,
                                 int16_t max_x2, int16_t *value_out);
bool telit_view_copy_exact(char *dst, size_t dst_cap,
                           telit_csv_view_t field);
bool telit_view_equals(telit_csv_view_t field, const char *expected);
bool telit_view_copy_hex(char *dst, size_t dst_cap, telit_csv_view_t field);
bool telit_view_digits_only(telit_csv_view_t field, size_t min_len,
                            size_t max_len);
bool telit_view_parse_plmn_spaced(telit_csv_view_t field, char mcc[4],
                                  char mnc[4]);
modem_diag_line_result_t telit_parse_scan_timer_value(
    const char *line, uint16_t *seconds);
modem_diag_line_result_t telit_parse_band_mode_value(
    const char *line, uint8_t *mode);
modem_diag_line_result_t telit_parse_tuner_enabled_value(
    const char *line, bool *enabled);

bool telit_init_parse_stune_capability(const char *line);
modem_provision_line_t telit_provision_stune_discover(const char *line);
modem_provision_line_t telit_provision_stune_disabled(const char *line);
void telit_tune_readback_begin(void);
telit_tune_rf_t telit_tune_rf_from_controls(uint8_t ctrl1, uint8_t ctrl2);
modem_provision_line_t telit_provision_gtune_line(const char *line);
bool telit_tune_readback_exact(void);
modem_provision_line_t telit_tune_discovery_finish(bool command_ok,
                                                    bool timed_out);
modem_provision_line_t telit_tune_row_finish(bool command_ok,
                                              bool timed_out);
modem_provision_line_t telit_tune_final_finish(bool command_ok,
                                                bool timed_out);
bool telit_tune_table_discovery_applicable(void);
bool telit_tune_repair_applicable(void);
void telit_tune_readback_target_rf1(void);
void telit_tune_readback_target_rf2(void);
void telit_tune_readback_target_rf3(void);
void telit_tune_readback_target_rf4(void);
bool telit_tune_rf1_applicable(void);
bool telit_tune_rf2_applicable(void);
bool telit_tune_rf3_applicable(void);
bool telit_tune_rf4_applicable(void);
uint64_t telit_tune_policy_mask(telit_tune_rf_t rf);
uint64_t telit_tune_policy_union(void);
uint64_t telit_tune_effective_mask(telit_tune_rf_t rf);
bool telit_tune_controls(telit_tune_rf_t rf, bool *ctrl1, bool *ctrl2);
bool telit_tune_build_row(telit_tune_rf_t rf, char *out, size_t out_cap);
bool telit_tune_build_rf1(char *out, size_t out_cap);
bool telit_tune_build_rf2(char *out, size_t out_cap);
bool telit_tune_build_rf3(char *out, size_t out_cap);
bool telit_tune_build_rf4(char *out, size_t out_cap);
modem_provision_line_t telit_provision_gpio2_alt16(const char *line);
modem_provision_line_t telit_provision_gpio3_alt17(const char *line);
uint64_t telit_tune_supported_mask(void);
bool telit_tune_repair_required(void);

bool telit_parse_ecam(const char *line, modem_call_event_t *out);
bool telit_build_call_command(call_txn_kind_t kind, const char *number,
                              uint8_t target_id, char *out, size_t out_cap,
                              uint32_t *timeout_ms);
bool telit_build_dtmf_command(char symbol, char *out, size_t out_cap,
                              uint32_t *timeout_ms);
uint8_t telit_call_forward_step_count(const call_forward_request_t *request);
bool telit_build_call_forward_step(const call_forward_request_t *request,
                                   uint8_t step, char *out, size_t out_cap);
bool telit_parse_call_forward_row(const char *line, call_forward_row_t *out);
bool telit_parse_voice_mailbox_row(const char *line,
                                   modem_voice_mailbox_row_t *out);
bool telit_parse_qss_query(const char *line, telit_qss_state_t *state);
bool telit_parse_qss_urc(const char *line, uint8_t *status_out);
modem_sim_observation_t telit_parse_sim_observation(const char *line);
bool telit_parse_mwi_urc(const char *line, telit_mwi_state_t *state);
bool telit_parse_mwi_query(const char *line, telit_mwi_state_t *state);
modem_message_waiting_row_result_t telit_parse_message_waiting_row(
    const char *line, modem_aux_event_t *out);
bool telit_parse_temperature(const char *line,
                             telit_temperature_t *temperature);
bool telit_parse_ismscfg(const char *line, uint8_t *mode_out);
bool telit_parse_fwswitch(const char *line, telit_fwswitch_t *state);
bool telit_parse_fwautosim(const char *line, uint8_t *mode_out);
bool telit_parse_aux_urc(const char *line, modem_aux_event_t *out);

bool telit_parse_signal_response(const char *line,
                                 modem_signal_sample_t *out);
modem_diag_line_result_t telit_diag_parse_rfsts(
    const char *line, modem_diag_snapshot_t *shadow);
modem_diag_line_result_t telit_diag_parse_moni(
    const char *line, modem_diag_snapshot_t *shadow);
modem_diag_line_result_t telit_diag_parse_stune_enabled(
    const char *line, modem_diag_snapshot_t *shadow);
modem_diag_line_result_t telit_diag_parse_stune_capability(
    const char *line, modem_diag_snapshot_t *shadow);
modem_diag_line_result_t telit_diag_parse_selbndmode(
    const char *line, modem_diag_snapshot_t *shadow);
modem_diag_line_result_t telit_diag_parse_scan_config(
    const char *line, modem_diag_snapshot_t *shadow);
modem_diag_line_result_t telit_diag_parse_gtune(
    const char *line, modem_diag_snapshot_t *shadow);
modem_diag_line_result_t telit_diag_parse_cgatt(
    const char *line, modem_diag_snapshot_t *shadow);
modem_diag_line_result_t telit_diag_parse_cgact(
    const char *line, modem_diag_snapshot_t *shadow);
modem_diag_line_result_t telit_diag_parse_cgcontrdp(
    const char *line, modem_diag_snapshot_t *shadow);
modem_diag_line_result_t telit_diag_parse_dvi(
    const char *line, modem_diag_snapshot_t *shadow);
modem_diag_line_result_t telit_diag_parse_dviext(
    const char *line, modem_diag_snapshot_t *shadow);
modem_diag_line_result_t telit_diag_parse_model(
    const char *line, modem_diag_snapshot_t *shadow);
modem_diag_line_result_t telit_diag_parse_firmware(
    const char *line, modem_diag_snapshot_t *shadow);
modem_diag_line_result_t telit_diag_parse_imei(
    const char *line, modem_diag_snapshot_t *shadow);
modem_diag_line_result_t telit_diag_parse_csms(
    const char *line, modem_diag_snapshot_t *shadow);
modem_diag_line_result_t telit_diag_parse_cnmi(
    const char *line, modem_diag_snapshot_t *shadow);
modem_diag_line_result_t telit_diag_parse_csmp(
    const char *line, modem_diag_snapshot_t *shadow);
modem_diag_line_result_t telit_diag_parse_csca(
    const char *line, modem_diag_snapshot_t *shadow);
modem_diag_line_result_t telit_diag_parse_csdh(
    const char *line, modem_diag_snapshot_t *shadow);
modem_diag_line_result_t telit_diag_parse_ismscfg_v2(
    const char *line, modem_diag_snapshot_t *shadow);
modem_diag_line_result_t telit_diag_parse_mwi_v2(
    const char *line, modem_diag_snapshot_t *shadow);
modem_diag_line_result_t telit_diag_parse_cpms_v2(
    const char *line, modem_diag_snapshot_t *shadow);
modem_diag_line_result_t telit_diag_parse_cpbs_v2(
    const char *line, modem_diag_snapshot_t *shadow);
bool telit_diag_group_finish(modem_diag_group_t group,
                             modem_diag_snapshot_t *shadow);
extern const modem_diag_query_t
    TELIT_DIAG_QUERIES[TELIT_DIAG_QUERY_COUNT];

bool telit_maintenance_build_scan_timer(uint16_t seconds, char *out,
                                        size_t out_cap);
modem_diag_line_result_t telit_maintenance_parse_scan_timer(
    const char *line, uint16_t *seconds);
bool telit_maintenance_build_band_mode(uint8_t mode, char *out,
                                       size_t out_cap);
modem_diag_line_result_t telit_maintenance_parse_band_mode(
    const char *line, uint8_t *mode);
modem_diag_line_result_t telit_maintenance_parse_band_nvm(
    const char *line, modem_band_config_t *config);
modem_diag_line_result_t telit_maintenance_parse_band_ram(
    const char *line, modem_band_config_t *config);
bool telit_maintenance_build_band_ram(const modem_band_config_t *config,
                                      char *out, size_t out_cap);
bool telit_maintenance_band_preset(
    uint8_t preset, const modem_band_config_t *active_config,
    modem_band_config_t *desired);
bool telit_maintenance_build_function(uint8_t mode, char *out,
                                      size_t out_cap);
modem_diag_line_result_t telit_maintenance_parse_function(
    const char *line, uint8_t *mode);
bool telit_maintenance_build_tuner_enabled(bool enabled, char *out,
                                           size_t out_cap);
modem_diag_line_result_t telit_maintenance_parse_tuner_enabled(
    const char *line, bool *enabled);
modem_diag_line_result_t telit_maintenance_parse_tuner_row(
    const char *line);
bool telit_maintenance_tuner_table_exact(void);
bool telit_maintenance_build_tuner_row(uint8_t rf_state, char *out,
                                       size_t out_cap);
bool telit_maintenance_antenna_controls(uint8_t rf_state, bool *a, bool *b);
bool telit_maintenance_build_gpio_query(uint8_t pin, char *out,
                                        size_t out_cap);
bool telit_maintenance_build_gpio_set(uint8_t pin, bool state,
                                      uint8_t direction, bool save,
                                      char *out, size_t out_cap);
modem_diag_line_result_t telit_maintenance_parse_gpio(
    const char *line, modem_gpio_config_t *config);

#endif /* MODEM_VENDOR_TELIT_INTERNAL_H */
