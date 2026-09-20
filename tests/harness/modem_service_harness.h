#ifndef MODEM_SERVICE_HARNESS_H
#define MODEM_SERVICE_HARNESS_H
/* Host harness for the selected modem backend.
 *
 * A test TU gets a fully-wired, host-runnable modem service through a SINGLE
 * include of this header: it defines a mock timebase, a queue-backed RX seam,
 * a TX-capture + auto-OK AT-responder, and every host stub that the separately
 * compiled service and selected backend reference, then exposes the mh_* driver:
 *
 *   mh_begin()      reset and initialize the selected backend at now = 0
 *   mh_feed(line)   enqueue one RX line ("modem -> firmware")
 *   mh_settle()     tick + auto-OK until the RX and request queues drain
 *   mh_advance(ms)  add ms to mock now, then settle
 *   mh_status()     snapshot of the projected modem_status_t
 *
 * Link modem_service.c and exactly one modem_vendor_*.c beside the test.
 * Exactly ONE test .c may include this stateful fixture header. Do NOT also link
 * tests/harness/modem_harness.c into the same TU -- that file re-defines the
 * same time_* / modem_uart_hal_* symbols (duplicate definitions). The
 * low-level harness_* API (tests/harness/modem_harness.h) is a separate seam.
 */
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#if (defined(SISU_MODEM_VENDOR_NONE) + defined(SISU_MODEM_VENDOR_TELIT)) != 1
#error "select a modem backend for the service harness"
#endif

#include "hal/modem_uart_hal.h"
#include "hal/modem_power_monitor_hal.h"
#include "hal/accessory_hal.h"
#include "services/timebase.h"
#include "services/log.h"
#include "services/core1_services.h"
#include "services/shared_3v8_service.h"
#include "storage/store_service.h"
#include "audio/nau88c22_codec.h"
#include "services/sms_picture_codec.h"
#include "services/message_service.h"
#include "services/modem_service.h"
#include "services/modem_vendor.h"
#include "../../src/services/modem_service_test.h"

/* ---- mock timebase (backs services/timebase.h) ---- */
static uint32_t s_mh_now;
uint32_t time_ms(void)     { return s_mh_now; }
uint64_t time_ms64(void)   { return s_mh_now; }
uint32_t time_ticks8(void) { return s_mh_now / 8u; }
int32_t  time_diff_ms(uint32_t a, uint32_t b) { return (int32_t)(a - b); }

/* ---- queue-backed RX (modem -> firmware); reset per mh_begin ---- */
#define MH_RX_CAP 16384u
static uint8_t s_mh_rx[MH_RX_CAP];
static size_t  s_mh_rx_len, s_mh_rx_pos;
static bool s_mh_rx_stuck_readable;
static uint32_t s_mh_rx_line_errors;
static void mh_rx_push_raw(const uint8_t *data, size_t len) {
    if (data == NULL || len > MH_RX_CAP - s_mh_rx_len) { return; }
    memcpy(&s_mh_rx[s_mh_rx_len], data, len);
    s_mh_rx_len += len;
}
static void mh_rx_push(const char *line) {
    size_t n = strlen(line);
    if (s_mh_rx_len + n + 2u > MH_RX_CAP) { return; }   /* bounded: drop on overflow */
    memcpy(&s_mh_rx[s_mh_rx_len], line, n);
    s_mh_rx_len += n;
    s_mh_rx[s_mh_rx_len++] = '\r';
    s_mh_rx[s_mh_rx_len++] = '\n';
}
uint32_t modem_uart_hal_read_available(uint8_t *dst, uint32_t max_len) {
    uint32_t n = 0u;
    while (n < max_len && s_mh_rx_pos < s_mh_rx_len) { dst[n++] = s_mh_rx[s_mh_rx_pos++]; }
    if (n == 0u && s_mh_rx_stuck_readable && dst != NULL) {
        memset(dst, 'X', max_len);
        return max_len;
    }
    return n;
}
uint32_t modem_uart_hal_rx_dropped(void)     { return 0u; }
uint32_t modem_uart_hal_rx_overruns(void)    { return 0u; }
uint32_t modem_uart_hal_rx_line_errors(void) { return s_mh_rx_line_errors; }
uint32_t modem_uart_hal_tx_stall_drops(void) { return 0u; }
bool modem_uart_hal_tx_idle(void) { return true; }
bool modem_uart_hal_rx_idle(void) {
    return s_mh_rx_pos == s_mh_rx_len && !s_mh_rx_stuck_readable;
}

/* ---- TX capture (firmware -> modem): commands, minus CR/LF ---- */
#define MH_TX_HISTORY_CAP 128u
#define MH_TX_EVENT_CAP 512u
#define MH_TX_EVENT_DATA_CAP 512u
typedef enum {
    MH_TX_EVENT_CSTR = 0,
    MH_TX_EVENT_RAW,
} mh_tx_event_kind_t;
typedef struct {
    mh_tx_event_kind_t kind;
    uint32_t at_ms;
    size_t len;
    bool truncated;
    uint8_t data[MH_TX_EVENT_DATA_CAP];
} mh_tx_event_t;
static char s_mh_tx_last[256];
static char s_mh_tx_history[MH_TX_HISTORY_CAP][256];
static size_t s_mh_tx_history_count;
static mh_tx_event_t s_mh_tx_events[MH_TX_EVENT_CAP];
static size_t s_mh_tx_event_count;
static uint32_t s_mh_uart_init_count;
static uint32_t s_mh_uart_irq_enable_count;
static uint32_t s_mh_uart_write_count;
static uint32_t s_mh_uart_no_cts_write_count;
static uint32_t s_mh_power_pin_count;
static uint32_t s_mh_graceful_shutdown_pulse_count;
static uint32_t s_mh_emergency_shutdown_pulse_count;
static uint32_t s_mh_last_shutdown_pulse_width_ms;
static uint32_t s_mh_uart_park_count;
static uint32_t s_mh_power_monitor_init_count;
static uint32_t s_mh_rail_set_count;
static bool s_mh_rail_enabled;
static uint32_t s_mh_rail_transition_count;
static bool s_mh_power_pin_asserted;
static bool s_mh_emergency_pin_asserted;
static bool s_mh_shutdown_alarm_active;
static modem_shutdown_pulse_t s_mh_shutdown_alarm_pulse;
static uint32_t s_mh_shutdown_alarm_deadline_ms;
static bool s_mh_graceful_shutdown_works;
static bool s_mh_emergency_shutdown_works;
static bool s_mh_shutdown_pulse_arm_works;
static bool s_mh_module_boots;
static bool s_mh_dtr_sleep_permitted;
static bool s_mh_dtr_wake_works;
static bool s_mh_cts_asserted;
static bool s_mh_rx_idle_high;
static bool s_mh_ri_asserted;
static bool s_mh_ri_wake_pending;
static bool s_mh_supply_pg;
static bool s_mh_status_valid;
static uint16_t s_mh_status_raw;
static uint16_t s_mh_status_mv;
static bool s_mh_status_restore_pending;
static uint32_t s_mh_status_restore_ms;
static bool s_mh_status_drop_pending;
static uint32_t s_mh_status_drop_ms;
static uint32_t s_mh_status_drop_duration_ms;
static bool s_mh_sim_present;
static bool s_mh_sim_ready;
/* Some functional levels deactivate the SIM interface even when a card is
 * physically present. Tests can model that distinction without changing the
 * physical-card fixture. */
static bool s_mh_sim_interface_active;
typedef void (*mh_response_hook_t)(const char *command);
static mh_response_hook_t s_mh_response_hook;
typedef void (*mh_raw_write_hook_t)(const uint8_t *data, size_t len);
static mh_raw_write_hook_t s_mh_raw_write_hook;
typedef enum {
    MH_FINAL_OK = 0,
    MH_FINAL_ERROR,
    MH_FINAL_NONE,
} mh_final_t;
/* Reset to OK for every captured command before the response hook runs. A
 * fault-injection hook may replace it with ERROR or suppress the final to
 * exercise the real command timeout path. */
static mh_final_t s_mh_final;

static void mh_capture_tx_event(mh_tx_event_kind_t kind,
                                const uint8_t *data, size_t len) {
    if (data == NULL || len == 0u || s_mh_tx_event_count >= MH_TX_EVENT_CAP) {
        return;
    }
    mh_tx_event_t *event = &s_mh_tx_events[s_mh_tx_event_count++];
    memset(event, 0, sizeof(*event));
    event->kind = kind;
    event->at_ms = s_mh_now;
    event->truncated = len > sizeof(event->data);
    event->len = event->truncated ? sizeof(event->data) : len;
    memcpy(event->data, data, event->len);
}

static void mh_capture_tx_command(const char *text) {
    if (text != NULL && text[0] != '\0' && text[0] != '\r') {
        strncpy(s_mh_tx_last, text, sizeof(s_mh_tx_last) - 1u);
        s_mh_tx_last[sizeof(s_mh_tx_last) - 1u] = '\0';
        if (s_mh_tx_history_count < MH_TX_HISTORY_CAP) {
            strncpy(s_mh_tx_history[s_mh_tx_history_count], text,
                    sizeof(s_mh_tx_history[0]) - 1u);
            s_mh_tx_history[s_mh_tx_history_count][sizeof(s_mh_tx_history[0]) - 1u] = '\0';
            s_mh_tx_history_count++;
        }
    }
}

void modem_uart_hal_write(const uint8_t *data, size_t len) {
    s_mh_uart_write_count++;
    mh_capture_tx_event(MH_TX_EVENT_RAW, data, len);
    if (s_mh_raw_write_hook != NULL) {
        s_mh_raw_write_hook(data, len);
    }
}
void modem_uart_hal_write_cstr(const char *text) {
    s_mh_uart_write_count++;
    if (text != NULL) {
        mh_capture_tx_event(MH_TX_EVENT_CSTR, (const uint8_t *)text,
                            strlen(text));
    }
    mh_capture_tx_command(text);
}
void modem_uart_hal_write_cstr_no_cts(const char *text) {
    s_mh_uart_write_count++;
    s_mh_uart_no_cts_write_count++;
    if (text != NULL) {
        mh_capture_tx_event(MH_TX_EVENT_CSTR, (const uint8_t *)text,
                            strlen(text));
    }
    mh_capture_tx_command(text);
}
void modem_uart_hal_init(void) {
    s_mh_uart_init_count++;
    s_mh_cts_asserted = s_mh_status_raw >= 1024u &&
        s_mh_dtr_wake_works && !s_mh_dtr_sleep_permitted;
}
void modem_uart_hal_enable_rx_irq(void) {
    s_mh_uart_irq_enable_count++;
}
void modem_uart_hal_set_power_pin(bool high) {
    s_mh_power_pin_count++;
    if (high) {
        s_mh_power_pin_asserted = true;
    } else if (s_mh_power_pin_asserted) {
        s_mh_power_pin_asserted = false;
        if (s_mh_module_boots) {
            s_mh_status_valid = true;
            s_mh_status_raw = 2214u;
            s_mh_status_mv = 1784u;
            s_mh_rx_idle_high = true;
            s_mh_cts_asserted = true;
        }
    }
}
bool modem_uart_hal_start_shutdown_pulse(modem_shutdown_pulse_t pulse,
                                         uint32_t width_ms) {
    if (!s_mh_shutdown_pulse_arm_works || width_ms == 0u ||
        s_mh_power_pin_asserted || s_mh_emergency_pin_asserted ||
        (pulse != MODEM_SHUTDOWN_PULSE_GRACEFUL &&
         pulse != MODEM_SHUTDOWN_PULSE_UNCONDITIONAL)) {
        return false;
    }
    s_mh_last_shutdown_pulse_width_ms = width_ms;
    s_mh_shutdown_alarm_active = true;
    s_mh_shutdown_alarm_pulse = pulse;
    s_mh_shutdown_alarm_deadline_ms = s_mh_now + width_ms;
    if (pulse == MODEM_SHUTDOWN_PULSE_GRACEFUL) {
        s_mh_graceful_shutdown_pulse_count++;
        s_mh_power_pin_asserted = true;
        if (s_mh_graceful_shutdown_works) {
            s_mh_status_raw = 36u;
            s_mh_status_mv = 29u;
            s_mh_rx_idle_high = false;
            s_mh_cts_asserted = false;
        }
        return true;
    }
    if (pulse == MODEM_SHUTDOWN_PULSE_UNCONDITIONAL) {
        s_mh_emergency_shutdown_pulse_count++;
        s_mh_emergency_pin_asserted = true;
        if (s_mh_emergency_shutdown_works) {
            s_mh_status_raw = 36u;
            s_mh_status_mv = 29u;
            s_mh_rx_idle_high = false;
            s_mh_cts_asserted = false;
        }
        return true;
    }
    return false;
}
void modem_uart_hal_cancel_shutdown_pulse(void) {
    s_mh_shutdown_alarm_active = false;
    s_mh_shutdown_alarm_deadline_ms = 0u;
    s_mh_power_pin_asserted = false;
    s_mh_emergency_pin_asserted = false;
}
void modem_uart_hal_set_dtr_sleep_permitted(bool permitted) {
    s_mh_dtr_sleep_permitted = permitted;
    if (permitted && s_mh_status_raw >= 1024u) {
        s_mh_cts_asserted = false;
    } else if (!permitted && s_mh_dtr_wake_works &&
               s_mh_status_raw >= 1024u) {
        s_mh_cts_asserted = true;
    }
}
bool modem_uart_hal_cts_asserted(void) { return s_mh_cts_asserted; }
bool modem_uart_hal_rx_idle_high(void) { return s_mh_rx_idle_high; }
void modem_uart_hal_park(void) {
    s_mh_uart_park_count++;
    s_mh_cts_asserted = false;
}
bool modem_uart_hal_ri_asserted(void) { return s_mh_ri_asserted; }
bool modem_uart_hal_take_ri_wake(void) {
    bool pending = s_mh_ri_wake_pending;
    s_mh_ri_wake_pending = false;
    return pending;
}
bool modem_uart_hal_status_pin(void)  { return s_mh_supply_pg; }
void modem_power_monitor_hal_init(void) {
    s_mh_power_monitor_init_count++;
}
bool modem_power_monitor_hal_read(uint16_t *raw, uint16_t *pin_mv) {
    if (raw == NULL || pin_mv == NULL || !s_mh_status_valid) return false;
    if (s_mh_status_drop_pending &&
        time_diff_ms(s_mh_now, s_mh_status_drop_ms) >= 0) {
        s_mh_status_drop_pending = false;
        s_mh_status_raw = 36u;
        s_mh_status_mv = 29u;
        s_mh_rx_idle_high = false;
        s_mh_cts_asserted = false;
        s_mh_status_restore_pending = true;
        s_mh_status_restore_ms =
            s_mh_now + s_mh_status_drop_duration_ms;
    }
    if (s_mh_status_restore_pending &&
        time_diff_ms(s_mh_now, s_mh_status_restore_ms) >= 0) {
        s_mh_status_restore_pending = false;
        s_mh_status_raw = 2214u;
        s_mh_status_mv = 1784u;
        s_mh_rx_idle_high = true;
        s_mh_cts_asserted = true;
    }
    *raw = s_mh_status_raw;
    *pin_mv = s_mh_status_mv;
    return true;
}
void shared_3v8_service_set_required(shared_3v8_owner_t owner, bool required) {
    if (owner == SHARED_3V8_OWNER_MODEM && s_mh_rail_enabled != required) {
        s_mh_rail_enabled = required;
        s_mh_rail_transition_count++;
    }
    if (owner == SHARED_3V8_OWNER_MODEM) {
        s_mh_supply_pg = required;
    }
    s_mh_rail_set_count++;
}
bool shared_3v8_service_enabled(void) { return s_mh_rail_enabled; }
uint32_t shared_3v8_service_transition_count(void) {
    return s_mh_rail_transition_count;
}
bool power_sleep_boot_was_warm(void) { return false; }

/* ---- persistent setting seam used by guarded radio maintenance ---- */
static bool s_mh_store_ready;
static bool s_mh_store_flush_ok;
static bool s_mh_store_flush_leaves_dirty;
static bool s_mh_store_dirty;
static char s_mh_radio_recovery[STORE_TEXT_MAX + 1u];
static char s_mh_board_imei[STORE_WARRANTY_SERIAL_MAX + 1u];
static const char *s_mh_cgsn_response;
static size_t s_mh_recovery_store_tx_count;
bool store_service_ready(void) { return s_mh_store_ready; }
static unsigned s_mh_picture_parts;
static unsigned s_mh_ringtone_parts;
static store_status_t s_mh_ringtone_result;
static char s_mh_local_pdu[SMS_DELIVER_HEX_MAX];
static unsigned s_mh_local_received, s_mh_local_sent, s_mh_local_lost;
static bool s_mh_local_reject;
static bool s_mh_local_commit_held;
static uint32_t s_mh_receive_receipt;
bool message_service_receive(const char *pdu) {
    if (s_mh_local_reject) { s_mh_local_lost++; return false; }
    snprintf(s_mh_local_pdu, sizeof(s_mh_local_pdu), "%s", pdu);
    s_mh_local_received++;
    return true;
}
bool message_service_receive_tracked(const char *pdu, uint32_t *receipt) {
    if (!message_service_receive(pdu)) return false;
    *receipt = ++s_mh_receive_receipt;
    return true;
}
bool message_service_receive_committed(uint32_t receipt) {
    return receipt != 0u && receipt == s_mh_receive_receipt && !s_mh_local_commit_held;
}
void message_service_receive_forget(uint32_t receipt) { (void)receipt; }
store_status_t store_picture_received_pdu_status(const char *pdu) {
    (void)pdu;
    return s_mh_local_commit_held ? STORE_STATUS_NOT_READY : STORE_STATUS_OK;
}
bool message_service_sent(const char *address, const char *text) {
    (void)address; (void)text;
    if (s_mh_local_reject) { s_mh_local_lost++; return false; }
    s_mh_local_sent++;
    return true;
}
void message_service_note_receive_loss(void) { s_mh_local_lost++; }
void message_service_get_status(message_status_t *out) {
    memset(out, 0, sizeof(*out));
    out->ready = true;
    out->received = s_mh_local_received;
    out->receive_errors = s_mh_local_lost;
}
store_status_t store_picture_receive_pdu(const char *pdu, uint32_t now_ms) {
    (void)now_ms;
    sms_codec_message_t part;
    if (!sms_pdu_decode(pdu, &part) || !part.has_ports ||
        part.dest_port != SMS_CODEC_PICTURE_PORT) return STORE_STATUS_NOT_FOUND;
    s_mh_picture_parts++;
    return STORE_STATUS_OK;
}
store_status_t store_ringtone_receive_pdu(const char *pdu, uint32_t now_ms) {
    (void)now_ms;
    sms_codec_message_t part;
    if (!sms_pdu_decode(pdu, &part) || !part.has_ports || part.dest_port != 0x1581u)
        return STORE_STATUS_NOT_FOUND;
    s_mh_ringtone_parts++;
    return s_mh_ringtone_result;
}
store_status_t store_ringtone_received_pdu_status(const char *pdu) {
    (void)pdu;
    return s_mh_local_commit_held ? STORE_STATUS_NOT_READY : STORE_STATUS_OK;
}
bool store_service_flush_all(void) {
    if (!s_mh_store_flush_ok) return false;
    if (!s_mh_store_flush_leaves_dirty) s_mh_store_dirty = false;
    return true;
}
void store_service_get_diag(store_diag_snapshot_t *out) {
    if (out == NULL) return;
    memset(out, 0, sizeof(*out));
    out->ready = s_mh_store_ready;
    if (s_mh_store_dirty) {
        out->dirty_mask =
            (uint16_t)(1u << STORE_UNIT_SETTINGS_SYSTEM);
    }
}
store_status_t store_setting_get_text(store_setting_key_t key, char *out_text,
                                      uint8_t out_cap) {
    if (!s_mh_store_ready) return STORE_STATUS_NOT_READY;
    if (key != STORE_SETTING_SYSTEM_NETMON_RADIO_RECOVERY ||
        out_text == NULL || out_cap == 0u) {
        return STORE_STATUS_INVALID_ARGUMENT;
    }
    strncpy(out_text, s_mh_radio_recovery, (size_t)out_cap - 1u);
    out_text[out_cap - 1u] = '\0';
    return STORE_STATUS_OK;
}
store_status_t store_setting_set_text(store_setting_key_t key,
                                      const char *text) {
    if (!s_mh_store_ready) return STORE_STATUS_NOT_READY;
    if (key != STORE_SETTING_SYSTEM_NETMON_RADIO_RECOVERY || text == NULL ||
        strlen(text) > STORE_TEXT_MAX) {
        return STORE_STATUS_INVALID_ARGUMENT;
    }
    strcpy(s_mh_radio_recovery, text);
    s_mh_store_dirty = true;
    if (text[0] != '\0') {
        s_mh_recovery_store_tx_count = s_mh_tx_history_count;
    }
    return STORE_STATUS_OK;
}
bool store_board_imei_valid(const char *imei) {
    if (imei == NULL || strlen(imei) != STORE_WARRANTY_SERIAL_MAX) {
        return false;
    }
    unsigned sum = 0u;
    for (uint8_t i = 0u; i < STORE_WARRANTY_SERIAL_MAX; i++) {
        if (imei[i] < '0' || imei[i] > '9') return false;
        unsigned digit = (unsigned)(imei[i] - '0');
        if ((i & 1u) != 0u) {
            digit *= 2u;
            if (digit > 9u) digit -= 9u;
        }
        sum += digit;
    }
    return (sum % 10u) == 0u;
}
store_status_t store_board_imei_get(char *out_imei, size_t out_cap) {
    if (out_imei == NULL || out_cap < sizeof(s_mh_board_imei)) {
        return STORE_STATUS_INVALID_ARGUMENT;
    }
    out_imei[0] = '\0';
    if (!store_board_imei_valid(s_mh_board_imei)) {
        return STORE_STATUS_NOT_FOUND;
    }
    memcpy(out_imei, s_mh_board_imei, sizeof(s_mh_board_imei));
    return STORE_STATUS_OK;
}
store_status_t store_board_imei_provision(const char *imei) {
    if (!store_board_imei_valid(imei)) return STORE_STATUS_INVALID_ARGUMENT;
    if (store_board_imei_valid(s_mh_board_imei)) {
        return strcmp(s_mh_board_imei, imei) == 0
                   ? STORE_STATUS_OK
                   : STORE_STATUS_CONFLICT;
    }
    memcpy(s_mh_board_imei, imei, sizeof(s_mh_board_imei));
    return s_mh_store_ready ? STORE_STATUS_OK : STORE_STATUS_NOT_READY;
}

/* ---- accessory_hal ---- */
void accessory_hal_init(void) {}
void accessory_hal_poll(uint32_t now_ms) { (void)now_ms; }
bool accessory_hal_headset_inserted(void) { return false; }
bool accessory_hal_take_insert_change(bool *now_inserted) { (void)now_inserted; return false; }
bool accessory_hal_take_hook_press(void) { return false; }
uint16_t accessory_hal_hook_mv(void) { return 0u; }

/* ---- log / core1 / codec / sms ---- */
void log_set_level(log_level_t level) { (void)level; }
void log_write(log_level_t level, const char *tag, const char *fmt, ...) { (void)level; (void)tag; (void)fmt; }
void log_vwrite(log_level_t level, const char *tag, const char *fmt, va_list args) { (void)level; (void)tag; (void)fmt; (void)args; }
void core1_services_start(bool modem_voice_transport_available) {
    (void)modem_voice_transport_available;
}
void core1_post_command(core1_cmd_t cmd, uint16_t arg) { (void)cmd; (void)arg; }
void core1_services_codec_reset_call_volume(void) {}
void core1_post_audio_composer_packed(const uint8_t *data, uint16_t len, uint8_t level) { (void)data; (void)len; (void)level; }
void core1_post_audio_composer_packed_loop(const uint8_t *data, uint16_t len, uint8_t level) { (void)data; (void)len; (void)level; }
bool nau88c22_codec_set_route(nau_route_t route) { (void)route; return true; }
bool nau88c22_codec_set_mic_power(bool in_call, bool headset_inserted) { (void)in_call; (void)headset_inserted; return true; }

/* ---- auto-OK AT-responder: OK to every written command (incl. an empty-OK
 * AT+CLCC). Returns true iff it emitted a response line this call. ---- */
static bool mh_respond_to_tx(void) {
    if (s_mh_tx_last[0] == '\0') { return false; }
    s_mh_final = MH_FINAL_OK;
    if (strcmp(s_mh_tx_last, "AT+CGSN") == 0 &&
        s_mh_cgsn_response != NULL && s_mh_cgsn_response[0] != '\0') {
        mh_rx_push(s_mh_cgsn_response);
    } else if (strcmp(s_mh_tx_last, "AT+CPIN?") == 0) {
        if (!s_mh_sim_interface_active || !s_mh_sim_present) {
            mh_rx_push("+CME ERROR: SIM not inserted");
            s_mh_final = MH_FINAL_NONE;
        } else {
            mh_rx_push(s_mh_sim_ready ? "+CPIN: READY" : "+CPIN: SIM PIN");
        }
    }
    if (s_mh_response_hook != NULL) s_mh_response_hook(s_mh_tx_last);
    if (s_mh_final == MH_FINAL_OK) {
        mh_rx_push("OK");
    } else if (s_mh_final == MH_FINAL_ERROR) {
        mh_rx_push("ERROR");
    }
    s_mh_tx_last[0] = '\0';
    return true;
}

static inline size_t mh_tx_count_exact(const char *command) {
    size_t count = 0u;
    for (size_t i = 0u; i < s_mh_tx_history_count; i++) {
        if (strcmp(s_mh_tx_history[i], command) == 0) count++;
    }
    return count;
}

static inline void mh_clear_tx_capture(void) {
    s_mh_tx_last[0] = '\0';
    s_mh_tx_history_count = 0u;
    s_mh_tx_event_count = 0u;
    s_mh_uart_write_count = 0u;
    s_mh_uart_no_cts_write_count = 0u;
}

static inline size_t mh_tx_event_find(mh_tx_event_kind_t kind,
                                      const void *data, size_t len,
                                      size_t start) {
    if (data == NULL) { return SIZE_MAX; }
    for (size_t i = start; i < s_mh_tx_event_count; i++) {
        const mh_tx_event_t *event = &s_mh_tx_events[i];
        if (!event->truncated && event->kind == kind && event->len == len &&
            memcmp(event->data, data, len) == 0) {
            return i;
        }
    }
    return SIZE_MAX;
}

/* ---- driver (external linkage; see the Interfaces mh_* signatures) ---- */
void mh_settle(void) {
    for (int i = 0; i < 64; i++) {
        size_t rx_before = s_mh_rx_len - s_mh_rx_pos;
        modem_service_tick(s_mh_now);
        bool emitted = mh_respond_to_tx();
        size_t rx_after = s_mh_rx_len - s_mh_rx_pos;
        if (!emitted && rx_before == 0u && rx_after == 0u) { break; }  /* quiescent */
    }
}

void mh_begin(void) {
    s_mh_ringtone_parts = 0u;
    s_mh_ringtone_result = STORE_STATUS_OK;
    s_mh_local_pdu[0] = 0;
    s_mh_local_received = s_mh_local_sent = s_mh_local_lost = 0u;
    s_mh_local_reject = false;
    s_mh_local_commit_held = false;
    s_mh_receive_receipt = 0u;
    s_mh_now = 0u;
    s_mh_rx_len = s_mh_rx_pos = 0u;
    s_mh_rx_stuck_readable = false;
    s_mh_rx_line_errors = 0u;
    s_mh_tx_last[0] = '\0';
    s_mh_tx_history_count = 0u;
    s_mh_tx_event_count = 0u;
    s_mh_uart_init_count = 0u;
    s_mh_uart_irq_enable_count = 0u;
    s_mh_uart_write_count = 0u;
    s_mh_uart_no_cts_write_count = 0u;
    s_mh_power_pin_count = 0u;
    s_mh_graceful_shutdown_pulse_count = 0u;
    s_mh_emergency_shutdown_pulse_count = 0u;
    s_mh_last_shutdown_pulse_width_ms = 0u;
    s_mh_uart_park_count = 0u;
    s_mh_power_monitor_init_count = 0u;
    s_mh_rail_set_count = 0u;
    s_mh_rail_enabled = false;
    s_mh_rail_transition_count = 0u;
    s_mh_power_pin_asserted = false;
    s_mh_emergency_pin_asserted = false;
    s_mh_shutdown_alarm_active = false;
    s_mh_shutdown_alarm_pulse = MODEM_SHUTDOWN_PULSE_GRACEFUL;
    s_mh_shutdown_alarm_deadline_ms = 0u;
    s_mh_graceful_shutdown_works = true;
    s_mh_emergency_shutdown_works = true;
    s_mh_shutdown_pulse_arm_works = true;
    s_mh_module_boots = true;
    s_mh_dtr_sleep_permitted = false;
    s_mh_dtr_wake_works = true;
    s_mh_cts_asserted = false;
    s_mh_rx_idle_high = false;
    s_mh_ri_asserted = false;
    s_mh_ri_wake_pending = false;
    s_mh_supply_pg = false;
    s_mh_status_valid = true;
    s_mh_status_raw = 36u;
    s_mh_status_mv = 29u;
    s_mh_status_restore_pending = false;
    s_mh_status_restore_ms = 0u;
    s_mh_status_drop_pending = false;
    s_mh_status_drop_ms = 0u;
    s_mh_status_drop_duration_ms = 0u;
    s_mh_sim_present = true;
    s_mh_sim_ready = true;
    s_mh_sim_interface_active = true;
    s_mh_store_ready = true;
    s_mh_store_flush_ok = true;
    s_mh_store_flush_leaves_dirty = false;
    s_mh_store_dirty = false;
    s_mh_radio_recovery[0] = '\0';
    s_mh_board_imei[0] = '\0';
    s_mh_cgsn_response = "490154203237518";
    s_mh_recovery_store_tx_count = SIZE_MAX;
    s_mh_response_hook = NULL;
    s_mh_raw_write_hook = NULL;
    s_mh_final = MH_FINAL_OK;
    modem_service_init();
    mh_settle();
}

void mh_feed(const char *line) {
    mh_rx_push(line);
    mh_settle();
}

void mh_advance(uint32_t ms) {
    s_mh_now += ms;
    if (s_mh_shutdown_alarm_active &&
        (int32_t)(s_mh_now - s_mh_shutdown_alarm_deadline_ms) >= 0) {
        if (s_mh_shutdown_alarm_pulse == MODEM_SHUTDOWN_PULSE_GRACEFUL) {
            s_mh_power_pin_asserted = false;
        } else {
            s_mh_emergency_pin_asserted = false;
        }
        s_mh_shutdown_alarm_active = false;
    }
    mh_settle();
}

modem_status_t mh_status(void) {
    modem_status_t st;
    modem_service_get_status(&st);
    return st;
}

#endif /* MODEM_SERVICE_HARNESS_H */
