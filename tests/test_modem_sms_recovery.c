#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "services/modem_sms_recovery.h"

static modem_sms_recovery_t r;
static char command[48], header[400];
static const char *row = "+CMGR: \"REC UNREAD\",\"+15551234567\",\"\",\"26/09/19,12:00:00+00\",145,0,0,0,\"\",129,5";

static void send_command(const char *expected) {
    assert(modem_sms_recovery_command(&r, 0u, command, sizeof(command)));
    assert(strcmp(command, expected) == 0);
    assert(!modem_sms_recovery_command(&r, 0u, command, sizeof(command)));
}
static void select_store(void) {
    memset(&r, 0, sizeof(r));
    modem_sms_recovery_request(&r);
    send_command("AT+CPMS=\"ME\"");
    assert(modem_sms_recovery_line(&r, "+CPMS: 2,255,2,255,2,255", header, sizeof(header)));
    modem_sms_recovery_final(&r, true, false, 0u);
}
static void payload(const char *data) {
    assert(modem_sms_recovery_line(&r, row, header, sizeof(header)));
    assert(strncmp(header, "+CMT: \"", 7u) == 0 || strncmp(header, "+CMT:\"", 6u) == 0);
    modem_sms_recovery_payload(&r, MODEM_SMS_DIRECT_STEP_READY, data);
    modem_sms_recovery_final(&r, true, false, 0u);
}
int main(void) {
    select_store();
    send_command("AT+CMGR=1"); payload("001122");
    assert(r.step == MODEM_SMS_RECOVERY_STORE);
    assert(!modem_sms_recovery_command(&r, 0, command, sizeof(command)));
    modem_sms_recovery_committed(&r);
    send_command("AT+CMGR=1"); payload("001122");
    send_command("AT+CMGD=1,0");
    modem_sms_recovery_final(&r, true, false, 0);
    assert(r.recovered == 1u && r.remaining == 1u);
    send_command("AT+CMGR=2");
    modem_sms_recovery_final(&r, false, true, 0);
    assert(r.failures == 0u);
    send_command("AT+CMGR=3"); payload("334455");
    modem_sms_recovery_committed(&r);
    send_command("AT+CMGR=3"); payload("DIFFERENT");
    assert(r.step == MODEM_SMS_RECOVERY_NONE && r.recovered == 1u && r.failures == 1u);
    assert(!modem_sms_recovery_command(&r, 59999u, command, sizeof(command)));
    assert(modem_sms_recovery_command(&r, 60000u, command, sizeof(command)));
    assert(strcmp(command, "AT+CPMS=\"ME\"") == 0);

    select_store(); send_command("AT+CMGR=1"); payload("001122");
    modem_sms_recovery_reset(&r); /* SIM/session change invalidates the receipt. */
    modem_sms_recovery_committed(&r);
    assert(!modem_sms_recovery_command(&r, 0, command, sizeof(command)));

    select_store(); send_command("AT+CMGR=1");
    assert(modem_sms_recovery_line(&r, "+CMGR: \"STO SENT\",\"123\",\"\",129,17,0,0,167,\"\",129,5", header, sizeof(header)));
    modem_sms_recovery_payload(&r, MODEM_SMS_DIRECT_STEP_READY, "001122");
    modem_sms_recovery_final(&r, true, false, 0);
    assert(r.step == MODEM_SMS_RECOVERY_READ && r.index == 2u && r.recovered == 0u);

    select_store(); send_command("AT+CMGR=1"); payload("001122");
    modem_sms_recovery_committed(&r);
    send_command("AT+CMGR=1"); payload("001122");
    send_command("AT+CMGD=1,0");
    modem_sms_recovery_final(&r, false, false, 0);
    assert(r.recovered == 0u && r.failures == 1u);
    const char *bad_counts[] = {"+CPMS: 1,0,0,0,0,0", "+CPMS: 0,513,0,513,0,513",
        "+CPMS: 0,255", "+CPMS: -1,255,0,255,0,255", "+CPMS: 0,255,0,255,0,255,0"};
    for (unsigned i = 0; i < sizeof(bad_counts) / sizeof(bad_counts[0]); i++) {
        memset(&r, 0, sizeof(r)); modem_sms_recovery_request(&r);
        send_command("AT+CPMS=\"ME\"");
        modem_sms_recovery_line(&r, bad_counts[i], header, sizeof(header));
        modem_sms_recovery_final(&r, true, false, 0);
        assert(r.step == MODEM_SMS_RECOVERY_NONE && r.failures == 1u);
    }
    select_store(); send_command("AT+CMGR=1");
    modem_sms_recovery_line(&r, row, header, sizeof(header));
    modem_sms_recovery_payload(&r, MODEM_SMS_DIRECT_STEP_FILTERED, "112233");
    modem_sms_recovery_final(&r, true, false, 0);
    assert(r.step == MODEM_SMS_RECOVERY_STORE && r.filtered);
    modem_sms_recovery_committed(&r);
    send_command("AT+CMGR=1");
    modem_sms_recovery_line(&r, row, header, sizeof(header));
    modem_sms_recovery_payload(&r, MODEM_SMS_DIRECT_STEP_FILTERED, "445566");
    modem_sms_recovery_final(&r, true, false, 0);
    assert(r.step == MODEM_SMS_RECOVERY_NONE && r.recovered == 0u);

    select_store(); send_command("AT+CMGR=1"); payload("INVALID RINGTONE");
    modem_sms_recovery_rejected(&r, 0u);
    assert(r.failures == 1u && r.remaining == 1u && r.recovered == 0u);
    send_command("AT+CMGR=2"); payload("VALID SMS");
    modem_sms_recovery_committed(&r);
    send_command("AT+CMGR=2"); payload("VALID SMS");
    send_command("AT+CMGD=2,0");
    modem_sms_recovery_final(&r, true, false, 0u);
    assert(r.step == MODEM_SMS_RECOVERY_NONE && r.recovered == 1u && r.failures == 1u);
    assert(!modem_sms_recovery_command(&r, 60000u, command, sizeof(command)));
    modem_sms_recovery_rejected(&r, 60000u);
    assert(r.failures == 1u); /* No stale admission after a session change/end. */

    select_store(); send_command("AT+CMGR=1"); payload("TEMPORARILY UNAVAILABLE");
    modem_sms_recovery_defer(&r, 0u);
    assert(r.remaining == 2u && r.recovered == 0u && r.pending);
    assert(!modem_sms_recovery_command(&r, 59999u, command, sizeof(command)));
    assert(modem_sms_recovery_command(&r, 60000u, command, sizeof(command)));
    assert(strcmp(command, "AT+CPMS=\"ME\"") == 0);
    puts("PASS: durable modem SMS recovery sequencing");
}
