#include <stdio.h>
#include <string.h>

#include "services/sms_vvm_filter.h"

static int failures;

static void check(bool condition, const char *label) {
    if (!condition) {
        fprintf(stderr, "FAIL: %s\n", label);
        failures++;
    }
}

static bool text_match(uint16_t port, const char *text) {
    return sms_vvm_control_payload_is_recognized(
        true, port, (const uint8_t *)text, strlen(text));
}

int main(void) {
    check(text_match(20481u,
                     "//VVM:STATUS:st=R;rc=0;srv=vvm.example;"),
          "default OMTP STATUS is recognized on a carrier-selected port");
    check(text_match(5499u,
                     "//VZWVVM:SYNC:ev=NM;id=143;c=1;"),
          "carrier-prefixed OMTP SYNC is recognized");
    check(text_match(SMS_VVM_LEGACY_STATUS_PORT,
                     "STATE?state=Active;server=vvm.example;port=143;"),
          "legacy VVM provisioning state is recognized");
    check(text_match(SMS_VVM_LEGACY_SYNC_PORT,
                     "MBOXUPDATE?m=5;server=vvm.example;port=993;"),
          "legacy VVM mailbox update is recognized");
    check(text_match(SMS_VVM_LEGACY_SYNC_PORT,
                     "UNRECOGNIZED?cmd=STATUS"),
          "legacy VVM command error is recognized");
    check(text_match(
              SMS_VVM_LEGACY_SYNC_PORT,
              "vvm-a.operator.example:80?f=0&v=900&m=15550100&p=&S=I&"
              "s=443&i=5143/6143&t=1:15550100:A:B:C:D:2"),
          "legacy URL VVM notification is recognized without carrier identity");
    check(text_match(
              SMS_VVM_LEGACY_SYNC_PORT,
              "mail.voicemail.example:5400?f=0&v=400&m=441234567890&p=&"
              "s=5433&t=4:441234567890:A:region:node:client:46173"),
          "older URL VVM envelope is recognized across operators and versions");

    check(!sms_vvm_control_payload_is_recognized(
              false, SMS_VVM_LEGACY_STATUS_PORT,
              (const uint8_t *)"STATE?state=Active",
              strlen("STATE?state=Active")),
          "unported text is never consumed as application control");
    check(!text_match(0x158au, "STATE?state=Active"),
          "legacy syntax on the Nokia picture port is not consumed");
    check(!text_match(SMS_VVM_LEGACY_SYNC_PORT,
                      "MBOXUPDATE?not-a-field"),
          "malformed legacy payload fails open");
    check(!text_match(SMS_VVM_LEGACY_SYNC_PORT, "MBOXUPDATE?m="),
          "legacy payload with an empty required value fails open");
    check(!text_match(5499u, "//VVM:SYNC:id=143;c=1"),
          "OMTP SYNC without an event reason fails open");
    check(!text_match(5499u, "//VVM:STATUS:st="),
          "OMTP payload with an empty required value fails open");
    check(!text_match(5499u, "//ANDROID:SYNC:ev=NM"),
          "an unrelated double-slash protocol fails open");
    check(!text_match(5499u, "ordinary application data"),
          "unknown port-addressed data remains a Nokia Data message");
    check(!text_match(
              SMS_VVM_LEGACY_STATUS_PORT,
              "vvm-a.operator.example:80?f=0&v=900&m=15550100&p=&s=443&"
              "t=1:15550100:A:B:C:D:2"),
          "legacy URL envelope on the wrong application port fails open");
    check(!text_match(
              SMS_VVM_LEGACY_SYNC_PORT,
              "vvm-a.operator.example:80?f=0&v=900&m=15550100&s=443&"
              "t=1:15550100:A:B:C:D:2"),
          "legacy URL envelope missing its password slot fails open");
    check(!text_match(
              SMS_VVM_LEGACY_SYNC_PORT,
              "vvm-a.operator.example:imap?f=0&v=900&m=15550100&p=&s=443&"
              "t=1:15550100:A:B:C:D:2"),
          "legacy URL envelope with malformed authority fails open");
    check(!text_match(
              SMS_VVM_LEGACY_SYNC_PORT,
              "vvm-a.operator.example:999999999999?f=0&v=900&m=15550100&"
              "p=&s=443&t=1:15550100:A:B:C:D:2"),
          "legacy URL envelope with an overflowing server port fails open");
    check(!text_match(
              SMS_VVM_LEGACY_SYNC_PORT,
              "service.operator.example:80?action=notify&mailbox=15550100"),
          "unrelated application URL on the VVM port fails open");

    static const char binary_status[] = "//VVM:STATUS:st=R;rc=0";
    check(sms_vvm_control_payload_is_recognized(
              true, 5499u, (const uint8_t *)binary_status,
              strlen(binary_status)),
          "8-bit application payload is classified without text conversion");

    if (failures != 0) {
        return 1;
    }
    puts("PASS: SMS VVM control filter");
    return 0;
}
