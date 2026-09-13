#include "modem_harness.h"

#include <string.h>

#include "hal/modem_uart_hal.h"
#include "services/modem_line_framer.h"
#include "services/timebase.h"

#define HARNESS_RX_CAP        8192u
#define HARNESS_MAX_TX_LINES    32u
#define HARNESS_MAX_RULES       16u
#define HARNESS_MAX_PENDING     64u

/* ---- mock clock ---- */
static uint32_t s_now_ms;

/* ---- RX byte FIFO (modem -> firmware) ---- */
static uint8_t s_rx[HARNESS_RX_CAP];
static size_t  s_rx_head, s_rx_tail;

/* ---- TX capture (firmware -> modem) ---- */
static char   s_tx_partial[HARNESS_LINE_MAX];
static size_t s_tx_partial_len;
static char   s_tx_lines[HARNESS_MAX_TX_LINES][HARNESS_LINE_MAX];
static size_t s_tx_lines_head, s_tx_lines_count;

/* ---- AT-responder ---- */
typedef struct {
    bool     in_use, by_tick, fired;
    uint32_t trig_ms;
    char     cmd_prefix[HARNESS_PREFIX_MAX];
    harness_emit_t emits[HARNESS_MAX_EMITS];
    uint8_t  emit_count;
} harness_rule_t;
static harness_rule_t s_rules[HARNESS_MAX_RULES];

typedef struct {
    bool     in_use;
    uint32_t due_ms, seq;
    char     line[HARNESS_LINE_MAX];
} pending_emit_t;
static pending_emit_t s_pending[HARNESS_MAX_PENDING];
static uint32_t s_emit_seq;

/* ---- helpers ---- */
static void copy_str(char *dst, size_t cap, const char *src) {
    size_t l = (src != NULL) ? strlen(src) : 0u;
    if (cap == 0u) return;
    if (l > cap - 1u) l = cap - 1u;
    if (l > 0u) memcpy(dst, src, l);
    dst[l] = '\0';
}

static void rx_compact(void) {
    if (s_rx_head > 0u) {
        size_t rem = s_rx_tail - s_rx_head;
        if (rem > 0u) memmove(s_rx, s_rx + s_rx_head, rem);
        s_rx_tail = rem;
        s_rx_head = 0u;
    }
}

void harness_rx_feed_bytes(const uint8_t *data, uint32_t len) {
    if (data == NULL || len == 0u) return;
    if (s_rx_tail + len > HARNESS_RX_CAP) rx_compact();
    size_t room = HARNESS_RX_CAP - s_rx_tail;
    size_t n = ((size_t)len < room) ? (size_t)len : room;   /* bounded: drop overflow */
    if (n > 0u) { memcpy(s_rx + s_rx_tail, data, n); s_rx_tail += n; }
}

void harness_rx_feed_line(const char *line) {
    char tmp[HARNESS_LINE_MAX + 2u];
    size_t l = (line != NULL) ? strlen(line) : 0u;
    if (l > HARNESS_LINE_MAX - 1u) l = HARNESS_LINE_MAX - 1u;
    if (l > 0u) memcpy(tmp, line, l);
    tmp[l]      = '\r';
    tmp[l + 1u] = '\n';
    harness_rx_feed_bytes((const uint8_t *)tmp, (uint32_t)(l + 2u));
}

/* ---- emit scheduling ---- */
static void schedule_emit(uint32_t due_ms, const char *line) {
    for (uint32_t i = 0u; i < HARNESS_MAX_PENDING; i++) {
        if (!s_pending[i].in_use) {
            s_pending[i].in_use = true;
            s_pending[i].due_ms = due_ms;
            s_pending[i].seq    = s_emit_seq++;
            copy_str(s_pending[i].line, sizeof(s_pending[i].line), line);
            return;
        }
    }
}

static void responder_match_command(const char *line) {
    for (uint32_t i = 0u; i < HARNESS_MAX_RULES; i++) {
        harness_rule_t *r = &s_rules[i];
        if (!r->in_use || r->by_tick) continue;
        if (r->cmd_prefix[0] == '\0' ||
            strncmp(line, r->cmd_prefix, strlen(r->cmd_prefix)) == 0) {
            for (uint8_t k = 0u; k < r->emit_count; k++)
                schedule_emit(s_now_ms + r->emits[k].delay_ms, r->emits[k].line);
        }
    }
}

/* ---- TX capture ---- */
static void tx_push_line(void) {
    if (s_tx_partial_len == 0u) return;
    s_tx_partial[s_tx_partial_len] = '\0';
    if (s_tx_lines_count < HARNESS_MAX_TX_LINES) {
        size_t slot = (s_tx_lines_head + s_tx_lines_count) % HARNESS_MAX_TX_LINES;
        copy_str(s_tx_lines[slot], HARNESS_LINE_MAX, s_tx_partial);
        s_tx_lines_count++;
    }
    responder_match_command(s_tx_partial);
    s_tx_partial_len = 0u;
}

static void tx_feed(const uint8_t *data, size_t len) {
    for (size_t i = 0u; i < len; i++) {
        uint8_t b = data[i];
        if (b == '\r' || b == '\n') { tx_push_line(); continue; }
        if (s_tx_partial_len + 1u < HARNESS_LINE_MAX) s_tx_partial[s_tx_partial_len++] = (char)b;
        else tx_push_line();   /* bounded: finalize on overflow */
    }
}

bool harness_tx_take_line(char *out, size_t cap) {
    if (out == NULL || cap == 0u || s_tx_lines_count == 0u) return false;
    copy_str(out, cap, s_tx_lines[s_tx_lines_head]);
    s_tx_lines_head = (s_tx_lines_head + 1u) % HARNESS_MAX_TX_LINES;
    s_tx_lines_count--;
    return true;
}

/* ---- responder API ---- */
static harness_rule_t *rule_alloc(void) {
    for (uint32_t i = 0u; i < HARNESS_MAX_RULES; i++)
        if (!s_rules[i].in_use) return &s_rules[i];
    return NULL;
}

void harness_at_on_command(const char *cmd_prefix, const harness_emit_t *emits, uint8_t count) {
    harness_rule_t *r = rule_alloc();
    if (r == NULL) return;
    memset(r, 0, sizeof(*r));
    r->in_use  = true;
    r->by_tick = false;
    copy_str(r->cmd_prefix, sizeof(r->cmd_prefix), cmd_prefix);
    if (count > HARNESS_MAX_EMITS) count = HARNESS_MAX_EMITS;
    for (uint8_t k = 0u; k < count; k++) r->emits[k] = emits[k];
    r->emit_count = count;
}

void harness_at_inject_at(uint32_t trig_ms, const harness_emit_t *emits, uint8_t count) {
    harness_rule_t *r = rule_alloc();
    if (r == NULL) return;
    memset(r, 0, sizeof(*r));
    r->in_use  = true;
    r->by_tick = true;
    r->trig_ms = trig_ms;
    if (count > HARNESS_MAX_EMITS) count = HARNESS_MAX_EMITS;
    for (uint8_t k = 0u; k < count; k++) r->emits[k] = emits[k];
    r->emit_count = count;
}

void harness_pump(void) {
    /* fire due tick rules (one-shot) */
    for (uint32_t i = 0u; i < HARNESS_MAX_RULES; i++) {
        harness_rule_t *r = &s_rules[i];
        if (r->in_use && r->by_tick && !r->fired &&
            (int32_t)(r->trig_ms - s_now_ms) <= 0) {
            for (uint8_t k = 0u; k < r->emit_count; k++)
                schedule_emit(s_now_ms + r->emits[k].delay_ms, r->emits[k].line);
            r->fired = true;
        }
    }
    /* drain due emits in schedule order (smallest seq first) into the RX FIFO */
    for (;;) {
        int      best = -1;
        uint32_t best_seq = 0u;
        for (uint32_t i = 0u; i < HARNESS_MAX_PENDING; i++) {
            if (s_pending[i].in_use && (int32_t)(s_pending[i].due_ms - s_now_ms) <= 0) {
                if (best < 0 || s_pending[i].seq < best_seq) {
                    best = (int)i;
                    best_seq = s_pending[i].seq;
                }
            }
        }
        if (best < 0) break;
        harness_rx_feed_line(s_pending[(uint32_t)best].line);
        s_pending[(uint32_t)best].in_use = false;
    }
}

/* ---- RX line reassembler (the same production framer used by the service) ---- */
uint32_t harness_drain_rx_lines(harness_line_cb cb, void *ctx) {
    uint8_t  buf[128];
    modem_line_framer_t framer;
    uint32_t delivered = 0u;
    uint32_t count;
    modem_line_framer_reset(&framer);
    while ((count = modem_uart_hal_read_available(buf, (uint32_t)sizeof(buf))) > 0u) {
        for (uint32_t i = 0u; i < count; i++) {
            modem_line_framer_event_t event =
                modem_line_framer_feed(&framer, buf[i]);
            if (event == MODEM_LINE_FRAMER_LINE) {
                if (cb != NULL) {
                    cb(modem_line_framer_line(&framer), ctx);
                }
                delivered++;
            }
        }
    }
    return delivered;
}

/* ---- mock timebase (external linkage; satisfies services/timebase.h) ---- */
void     harness_time_set(uint32_t now_ms)     { s_now_ms = now_ms; }
void     harness_time_advance(uint32_t delta)  { s_now_ms += delta; }
uint32_t harness_time_now(void)                { return s_now_ms; }

uint32_t time_ms(void)                         { return s_now_ms; }
uint64_t time_ms64(void)                       { return (uint64_t)s_now_ms; }
uint32_t time_ticks8(void)                     { return s_now_ms / 8u; }
int32_t  time_diff_ms(uint32_t a, uint32_t b)  { return (int32_t)(a - b); }

/* ---- modem_uart_hal implementation (external linkage) ---- */
void modem_uart_hal_init(void) {}
void modem_uart_hal_enable_rx_irq(void) {}
void modem_uart_hal_set_dtr_sleep_permitted(bool permitted) { (void)permitted; }
bool modem_uart_hal_cts_asserted(void) { return true; }
void modem_uart_hal_park(void) {}
uint32_t modem_uart_hal_read_available(uint8_t *dst, uint32_t max_len) {
    size_t avail = s_rx_tail - s_rx_head;
    size_t n = (avail < (size_t)max_len) ? avail : (size_t)max_len;
    if (dst != NULL && n > 0u) memcpy(dst, s_rx + s_rx_head, n);
    s_rx_head += n;
    if (s_rx_head == s_rx_tail) { s_rx_head = 0u; s_rx_tail = 0u; }
    return (uint32_t)n;
}
uint32_t modem_uart_hal_rx_dropped(void)  { return 0u; }
uint32_t modem_uart_hal_rx_overruns(void) { return 0u; }
uint32_t modem_uart_hal_rx_line_errors(void) { return 0u; }
void modem_uart_hal_write(const uint8_t *data, size_t len) {
    if (data != NULL) tx_feed(data, len);
}
void modem_uart_hal_write_cstr(const char *text) {
    if (text != NULL) tx_feed((const uint8_t *)text, strlen(text));
}
void modem_uart_hal_write_cstr_no_cts(const char *text) {
    if (text != NULL) tx_feed((const uint8_t *)text, strlen(text));
}
uint32_t modem_uart_hal_tx_stall_drops(void) { return 0u; }
bool modem_uart_hal_tx_idle(void)            { return true; }
bool modem_uart_hal_rx_idle_high(void)       { return true; }
void modem_uart_hal_set_power_pin(bool high) { (void)high; }
bool modem_uart_hal_start_shutdown_pulse(modem_shutdown_pulse_t pulse,
                                         uint32_t width_ms) {
    (void)pulse;
    (void)width_ms;
    return false;
}
void modem_uart_hal_cancel_shutdown_pulse(void) {}
bool modem_uart_hal_ri_asserted(void)        { return false; }
bool modem_uart_hal_take_ri_wake(void)       { return false; }
bool modem_uart_hal_status_pin(void)         { return false; }
void modem_power_monitor_hal_init(void) {}
bool modem_power_monitor_hal_read(uint16_t *raw, uint16_t *pin_mv) {
    if (raw != NULL) *raw = 0u;
    if (pin_mv != NULL) *pin_mv = 0u;
    return false;
}

/* ---- reset ---- */
void harness_reset(void) {
    s_now_ms = 0u;
    s_rx_head = 0u; s_rx_tail = 0u;
    s_tx_partial_len = 0u;
    s_tx_lines_head = 0u; s_tx_lines_count = 0u;
    s_emit_seq = 0u;
    memset(s_rules, 0, sizeof(s_rules));
    memset(s_pending, 0, sizeof(s_pending));
    /* s_rx / s_tx_lines contents are irrelevant once the indices are reset. */
}
