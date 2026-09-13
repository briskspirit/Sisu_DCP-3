#ifndef MODEM_MAINTENANCE_H
#define MODEM_MAINTENANCE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "services/modem_diag.h"

#define MODEM_MAINTENANCE_COMMAND_MAX 96u
#define MODEM_MAINTENANCE_TIMEOUT_MS 5000u

typedef enum {
    MODEM_MAINTENANCE_NONE = 0,
    MODEM_MAINTENANCE_SCAN_TIMER,
    MODEM_MAINTENANCE_BAND_TEST,
    MODEM_MAINTENANCE_ANTENNA,
} modem_maintenance_action_t;

typedef enum {
    MODEM_MAINTENANCE_IDLE = 0,
    MODEM_MAINTENANCE_PENDING,
    MODEM_MAINTENANCE_RUNNING,
    MODEM_MAINTENANCE_ACTIVE,
    MODEM_MAINTENANCE_RESTORING,
    MODEM_MAINTENANCE_DONE,
    MODEM_MAINTENANCE_ERROR,
    /* Automatic tuner ownership could not be proven. RF remains disabled. */
    MODEM_MAINTENANCE_LOCKED,
} modem_maintenance_state_t;

typedef enum {
    MODEM_MAINTENANCE_ERROR_NONE = 0,
    MODEM_MAINTENANCE_ERROR_UNAVAILABLE,
    MODEM_MAINTENANCE_ERROR_BUSY,
    MODEM_MAINTENANCE_ERROR_ARGUMENT,
    MODEM_MAINTENANCE_ERROR_STORAGE,
    MODEM_MAINTENANCE_ERROR_COMMAND,
    MODEM_MAINTENANCE_ERROR_TIMEOUT,
    MODEM_MAINTENANCE_ERROR_READBACK,
    MODEM_MAINTENANCE_ERROR_CANCELLED,
} modem_maintenance_error_t;

typedef struct {
    uint32_t sequence;
    modem_maintenance_action_t action;
    modem_maintenance_state_t state;
    modem_maintenance_error_t error;
    uint8_t step;
    uint8_t retries;
    uint16_t scan_timer_s;
    uint8_t band_preset;       /* 1..5 = LTE B2/B4/B5/B12/B14 */
    uint8_t antenna_rf;        /* 1..4, zero while automatic */
    bool band_test_active;
    bool antenna_active;
    bool recovery_pending;
} modem_maintenance_snapshot_t;

typedef struct {
    uint8_t gsm;
    uint16_t wcdma;
    uint64_t lte;
} modem_band_config_t;

typedef struct {
    uint8_t direction;
    bool state;
} modem_gpio_config_t;

/* Backend command/readback boundary for guarded Net Monitor maintenance. The
 * engine owns admission-independent state, persistence, cancellation, retries,
 * and fail-closed sequencing; a modem backend owns syntax and grammar only. */
typedef struct {
    bool supported;
    /* Maximum SIM-lifecycle convergence time after a maintenance-owned
     * functional-level transition back online. */
    uint32_t sim_transition_window_ms;
    const char *scan_timer_query_cmd;
    bool (*build_scan_timer_set)(uint16_t seconds, char *out,
                                 size_t out_cap);
    modem_diag_line_result_t (*parse_scan_timer)(const char *line,
                                                 uint16_t *seconds);

    const char *band_mode_query_cmd;
    bool (*build_band_mode_set)(uint8_t mode, char *out, size_t out_cap);
    modem_diag_line_result_t (*parse_band_mode)(const char *line,
                                                uint8_t *mode);
    const char *band_nvm_query_cmd;
    const char *band_ram_query_cmd;
    bool (*build_band_ram_set)(const modem_band_config_t *config,
                               char *out, size_t out_cap);
    modem_diag_line_result_t (*parse_band_nvm)(const char *line,
                                               modem_band_config_t *config);
    modem_diag_line_result_t (*parse_band_ram)(const char *line,
                                               modem_band_config_t *config);
    bool (*band_preset)(uint8_t preset,
                        const modem_band_config_t *active_config,
                        modem_band_config_t *desired);

    const char *function_query_cmd;
    bool (*build_function_set)(uint8_t mode, char *out, size_t out_cap);
    modem_diag_line_result_t (*parse_function)(const char *line,
                                               uint8_t *mode);
    const char *tuner_enabled_query_cmd;
    bool (*build_tuner_enabled_set)(bool enabled, char *out,
                                    size_t out_cap);
    modem_diag_line_result_t (*parse_tuner_enabled)(const char *line,
                                                    bool *enabled);
    const char *tuner_table_query_cmd;
    void (*tuner_table_begin)(void);
    modem_diag_line_result_t (*parse_tuner_table_row)(const char *line);
    bool (*tuner_table_exact)(void);
    bool (*build_tuner_row)(uint8_t rf_state, char *out, size_t out_cap);

    uint8_t antenna_gpio_a;
    uint8_t antenna_gpio_b;
    uint8_t antenna_gpio_a_alt_direction;
    uint8_t antenna_gpio_b_alt_direction;
    bool (*antenna_controls)(uint8_t rf_state, bool *a, bool *b);
    bool (*build_gpio_query)(uint8_t pin, char *out, size_t out_cap);
    bool (*build_gpio_set)(uint8_t pin, bool state, uint8_t direction,
                           bool save, char *out, size_t out_cap);
    modem_diag_line_result_t (*parse_gpio)(const char *line,
                                          modem_gpio_config_t *config);
} modem_maintenance_backend_t;

/* Platform hooks deliberately carry no modem syntax. The engine calls write
 * before flush, marks RAM recovery ownership between them, and brackets its
 * own CFUN transitions so the outer modem service can guard SIM observations. */
typedef struct {
    bool (*write_recovery_record)(const char *record);
    bool (*flush_recovery_record)(void);
    void (*hold_sim_offline)(void);
    void (*begin_sim_online)(uint32_t now_ms);
} modem_maintenance_hooks_t;

typedef struct {
    char command[MODEM_MAINTENANCE_COMMAND_MAX];
    uint32_t timeout_ms;
} modem_maintenance_dispatch_t;

typedef enum {
    MODEM_MAINTENANCE_RECOVERY_EMPTY = 0,
    MODEM_MAINTENANCE_RECOVERY_RESTORE_STARTED,
    MODEM_MAINTENANCE_RECOVERY_FAIL_CLOSED,
} modem_maintenance_recovery_result_t;

void modem_maintenance_init(const modem_maintenance_backend_t *backend,
                            const modem_maintenance_hooks_t *hooks);
void modem_maintenance_reset_session(void);
bool modem_maintenance_supported(void);
bool modem_maintenance_can_admit(void);
bool modem_maintenance_request_scan_timer(uint16_t seconds);
bool modem_maintenance_read_scan_timer(void);
bool modem_maintenance_start_band_test(uint8_t preset);
bool modem_maintenance_restore_band(bool command_in_flight);
bool modem_maintenance_enter_antenna(void);
bool modem_maintenance_select_antenna(uint8_t rf_state);
bool modem_maintenance_exit_antenna(bool command_in_flight);
void modem_maintenance_cancel(bool command_in_flight);
void modem_maintenance_cancel_for_call(bool command_in_flight);
void modem_maintenance_get_snapshot(modem_maintenance_snapshot_t *out);

bool modem_maintenance_recovery_checked(void);
modem_maintenance_recovery_result_t modem_maintenance_recover_record(
    const char *record, bool command_in_flight);
bool modem_maintenance_has_sequence(void);
bool modem_maintenance_blocks_other_work(void);
bool modem_maintenance_blocks_sleep(void);
bool modem_maintenance_holds_cfun4(void);

bool modem_maintenance_prepare_next(uint32_t now_ms,
                                    modem_maintenance_dispatch_t *out);
bool modem_maintenance_parse_expected_line(const char *line);
void modem_maintenance_finish_command(bool ok, bool timed_out,
                                      uint32_t now_ms);

#endif
