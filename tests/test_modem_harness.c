/* Self-test for the black-box modem harness infra.
 * Oracle: production line framing plus the harness scheduling contract (a
 * command rule schedules emits at now+delay; a tick rule fires once at the
 * absolute trigger), not observed modem output. */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "harness/modem_harness.h"
#include "services/timebase.h"
#include "hal/modem_uart_hal.h"

static int s_failures;
static void assert_true(bool cond, const char *msg) {
    if (!cond) { fprintf(stderr, "FAIL: %s\n", msg); s_failures++; }
}

typedef struct { char lines[16][HARNESS_LINE_MAX]; uint32_t n; } collected_t;
static void collect_cb(const char *line, void *ctx) {
    collected_t *c = (collected_t *)ctx;
    if (c->n < 16u) {
        size_t l = strlen(line);
        if (l > HARNESS_LINE_MAX - 1u) l = HARNESS_LINE_MAX - 1u;
        memcpy(c->lines[c->n], line, l);
        c->lines[c->n][l] = '\0';
        c->n++;
    }
}

/* (b) RX stub + reassembler: a fed 'RING' line is delivered verbatim. */
static void test_ring_delivery(void) {
    collected_t c = {0};
    harness_reset();
    harness_rx_feed_line("RING");
    uint32_t n = harness_drain_rx_lines(collect_cb, &c);
    assert_true(n == 1u, "one line delivered from a single fed RING");
    assert_true(c.n == 1u && strcmp(c.lines[0], "RING") == 0, "delivered line is exactly RING");
    collected_t c2 = {0};
    assert_true(harness_drain_rx_lines(collect_cb, &c2) == 0u, "RX empty after drain");
}

/* RX framing: CR ignored, chunk boundaries transparent, >chunk lines intact. */
static void test_rx_framing(void) {
    collected_t c = {0};
    harness_reset();
    /* two raw feeds that split "RING\r\n" across a boundary. */
    harness_rx_feed_bytes((const uint8_t *)"RI", 2u);
    harness_rx_feed_bytes((const uint8_t *)"NG\r\n", 4u);
    assert_true(harness_drain_rx_lines(collect_cb, &c) == 1u, "split feed reassembles one line");
    assert_true(strcmp(c.lines[0], "RING") == 0, "split feed reassembles RING");

    /* a line longer than the 128-byte read chunk survives intact. */
    collected_t c2 = {0};
    char big[200];
    memset(big, 'A', sizeof(big));
    big[199] = '\0';   /* 199 'A' */
    harness_reset();
    harness_rx_feed_line(big);
    assert_true(harness_drain_rx_lines(collect_cb, &c2) == 1u, "long line -> one delivery");
    assert_true(strlen(c2.lines[0]) == 199u, "long line length preserved across chunks");
}

/* (a) mock timebase. */
static void test_timebase(void) {
    harness_reset();
    harness_time_set(1000u);
    assert_true(harness_time_now() == 1000u && time_ms() == 1000u, "clock set");
    assert_true(time_ms64() == 1000u, "clock ms64");
    assert_true(time_ticks8() == 125u, "1000ms -> 125 8ms-ticks");
    harness_time_advance(500u);
    assert_true(time_ms() == 1500u, "clock advance");
    assert_true(time_diff_ms(1500u, 1000u) == 500, "time_diff_ms modular");
}

/* (c) AT-responder: parse a written command, emit scripted lines; a
 * tick-scheduled URC appears only after the clock reaches its trigger. */
static void test_responder(void) {
    harness_reset();
    harness_time_set(0u);

    harness_emit_t clcc[2] = {
        { 0u, "+CLCC: 1,0,0,0,0,\"+15551234567\",145" },
        { 0u, "OK" },
    };
    harness_at_on_command("AT+CLCC", clcc, 2u);

    harness_emit_t urc[1] = { { 0u, "RING" } };
    harness_at_inject_at(100u, urc, 1u);

    /* firmware writes the command. */
    modem_uart_hal_write_cstr("AT+CLCC\r");

    /* TX was captured as exactly one command line. */
    char tx[HARNESS_LINE_MAX];
    assert_true(harness_tx_take_line(tx, sizeof(tx)) && strcmp(tx, "AT+CLCC") == 0,
                "TX captured the written AT+CLCC command");
    assert_true(!harness_tx_take_line(tx, sizeof(tx)), "no second TX line");

    /* at t=0 the command emits (delay 0) are due; the URC (tick 100) is not. */
    collected_t c = {0};
    harness_pump();
    assert_true(harness_drain_rx_lines(collect_cb, &c) == 2u, "command response is two lines");
    assert_true(strcmp(c.lines[0], "+CLCC: 1,0,0,0,0,\"+15551234567\",145") == 0, "CLCC row first");
    assert_true(strcmp(c.lines[1], "OK") == 0, "OK second, after the CLCC row");

    /* URC has not fired yet. */
    collected_t c2 = {0};
    harness_pump();
    assert_true(harness_drain_rx_lines(collect_cb, &c2) == 0u, "URC not delivered before its tick");

    /* advance to the trigger; the URC fires exactly once. */
    harness_time_advance(100u);
    harness_pump();
    collected_t c3 = {0};
    assert_true(harness_drain_rx_lines(collect_cb, &c3) == 1u, "URC delivered at its tick");
    assert_true(strcmp(c3.lines[0], "RING") == 0, "URC line is RING");

    /* one-shot: further pumps deliver nothing. */
    harness_time_advance(100u);
    harness_pump();
    collected_t c4 = {0};
    assert_true(harness_drain_rx_lines(collect_cb, &c4) == 0u, "tick URC is one-shot");
}

int main(void) {
    test_ring_delivery();
    test_rx_framing();
    test_timebase();
    test_responder();
    if (s_failures == 0) printf("ALL HARNESS SELF-TESTS PASSED\n");
    return s_failures ? 1 : 0;
}
