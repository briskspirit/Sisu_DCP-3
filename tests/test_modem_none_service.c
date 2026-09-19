#include <stdio.h>

#include "harness/modem_service_harness.h"

static int s_failures;

static void check(bool condition, const char *message) {
    if (!condition) {
        fprintf(stderr, "FAIL: %s\n", message);
        s_failures++;
    }
}

static void check_admissions_rejected(void) {
    static const uint8_t payload[] = {0x01u, 0x02u};
    uint32_t sms_request_id = 99u;

    check(!modem_service_request_dial("123"), "dial rejected");
    check(!modem_service_request_answer(), "answer rejected");
    check(!modem_service_request_hangup(), "hangup rejected");
    check(!modem_service_request_dtmf('1'), "DTMF rejected");
    check(!modem_service_request_dtmf_sequence("123456"),
          "DTMF sequence rejected");
    check(!modem_service_request_call_waiting_answer(), "waiting answer rejected");
    check(!modem_service_request_call_waiting_reject(), "waiting reject rejected");
    check(!modem_service_request_call_swap(), "swap rejected");
    check(!modem_service_call_hold_available(), "hold unavailable");
    check(!modem_service_request_call_hold(), "hold rejected");
    check(!modem_service_request_call_release_active(), "release active rejected");
    check(!modem_service_call_release_active_pending(),
          "release-active ownership is absent");
    check(!modem_service_request_call_release_leg(1u), "release leg rejected");
    check(!modem_service_request_send_sms(
              "123", "test", &sms_request_id) && sms_request_id == 0u,
          "SMS rejected without an orphan token");
    sms_request_id = 99u;
    check(!modem_service_request_send_binary_sms("123", payload, sizeof(payload),
                                                  1u, 2u, &sms_request_id) &&
              sms_request_id == 0u,
          "binary SMS rejected without an orphan token");
    sms_request_id = 99u;
    check(!modem_service_request_save_sms(
              "123", "test", &sms_request_id) && sms_request_id == 0u,
          "save SMS rejected without an orphan token");
    check(!modem_service_request_debug_at("AT"), "raw AT rejected");
    sms_request_id = 99u;
    check(!modem_service_request_sms_mailbox(
              MODEM_SMS_MAILBOX_INBOX, &sms_request_id) &&
              sms_request_id == 0u,
          "mailbox rejected without an orphan token");
    uint16_t sms_indices[2] = {0u, 7u};
    sms_request_id = 99u;
    check(!modem_service_request_sms_read(
              sms_indices, 2u, false, 0x1234u, &sms_request_id) &&
              sms_request_id == 0u,
          "SMS on-demand read rejected without an orphan token");
    sms_request_id = 99u;
    check(!modem_service_request_delete_sms_indices(
              sms_indices, 2u, &sms_request_id) && sms_request_id == 0u,
          "multipart SMS delete rejected without an orphan token");
    check(!modem_service_request_send_sms("123", "test", NULL) &&
              !modem_service_request_sms_mailbox(
                  MODEM_SMS_MAILBOX_INBOX, NULL),
          "SMS admission requires an explicit request owner");
    modem_service_test_snapshot_t snapshot;
    modem_service_test_get_snapshot(&snapshot);
    check(snapshot.request_queue_depth == 0u,
          "no rejected request reached the queue");
}

int main(void) {
    mh_begin();

    modem_status_t status = mh_status();
    check(!status.available, "status explicitly reports unavailable");
    check(!status.at_ready && !status.sim_ready && !status.network_registered,
          "unavailable status stays offline");
    check(status.call_state == MODEM_CALL_IDLE, "unavailable call state is idle");
    check(modem_service_is_powered_off(), "unavailable backend is logically off");
    check(s_mh_uart_init_count == 0u && s_mh_rail_set_count == 0u,
          "init does not touch transport or rail");

    check_admissions_rejected();
    modem_service_new_call_abandoned();
    check(!modem_service_new_call_cleanup_pending(), "no cleanup can be pending");

    for (unsigned i = 0u; i < 4u; i++) {
        modem_service_power_on();
        modem_service_tick(i * 1000u);
        modem_service_power_off();
    }
    check(s_mh_uart_init_count == 0u, "power cycling never initializes UART");
    check(s_mh_uart_write_count == 0u, "power cycling never transmits");
    check(s_mh_power_pin_count == 0u, "power cycling never pulses control");
    check(s_mh_rail_set_count == 0u, "power cycling never changes rail");

    if (s_failures == 0) {
        printf("test_modem_none_service: OK\n");
    }
    return s_failures != 0;
}
