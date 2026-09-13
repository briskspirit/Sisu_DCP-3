#ifndef MODEM_HARNESS_H
#define MODEM_HARNESS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define HARNESS_LINE_MAX   256u
#define HARNESS_PREFIX_MAX  48u
#define HARNESS_MAX_EMITS    8u

/* One scripted response line, emitted delay_ms after its trigger fires.
 * `line` carries the payload WITHOUT trailing CR/LF (the harness appends them). */
typedef struct {
    uint32_t delay_ms;
    char     line[HARNESS_LINE_MAX];
} harness_emit_t;

/* Callback for the RX line reassembler. */
typedef void (*harness_line_cb)(const char *line, void *ctx);

/* Lifecycle: zero the clock, both FIFOs, and all responder rules. */
void harness_reset(void);

/* Mock timebase (backs time_ms / time_ms64 / time_ticks8 / time_diff_ms). */
void     harness_time_set(uint32_t now_ms);
void     harness_time_advance(uint32_t delta_ms);
uint32_t harness_time_now(void);

/* RX injection: bytes/lines the "modem" sends toward the firmware. */
void harness_rx_feed_bytes(const uint8_t *data, uint32_t len);
void harness_rx_feed_line(const char *line);       /* appends "\r\n" */

/* TX capture: complete CR/LF-delimited command lines the firmware wrote. */
bool harness_tx_take_line(char *out, size_t cap);  /* FIFO pop; false if none */

/* AT-responder scripting. A command rule fires each time a written command
 * line begins with cmd_prefix ("" = every command); a tick rule fires ONCE
 * when the mock clock reaches trig_ms. Both schedule their emits at
 * now+delay_ms; harness_pump() drains due emits into the RX queue. */
void harness_at_on_command(const char *cmd_prefix, const harness_emit_t *emits, uint8_t count);
void harness_at_inject_at(uint32_t trig_ms, const harness_emit_t *emits, uint8_t count);
void harness_pump(void);

/* Line reassembler over the RX stub, backed by the production framer. Returns
 * the number of complete in-bound lines delivered. */
uint32_t harness_drain_rx_lines(harness_line_cb cb, void *ctx);

#endif /* MODEM_HARNESS_H */
