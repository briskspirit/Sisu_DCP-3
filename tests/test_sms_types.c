#include <stdio.h>

#include "services/sms_types.h"
#include "services/message_types.h"

_Static_assert(MODEM_SMS_TEXT_MAX == 160u, "SMS text capacity changed");
_Static_assert(MODEM_SMS_DECODED_TEXT_MAX == 320u,
               "decoded SMS UTF-8 capacity changed");
_Static_assert(MODEM_SMS_BINARY_MAX == 384u, "SMS binary capacity changed");
_Static_assert(MODEM_SMS_BINARY_CHUNK_MAX == 128u,
               "SMS binary chunk capacity changed");
_Static_assert(MODEM_SMS_SENDER_MAX == 32u, "SMS sender capacity changed");
_Static_assert(MODEM_SMS_TIMESTAMP_MAX == 24u,
               "SMS timestamp capacity changed");
_Static_assert(MODEM_SMS_STATUS_MAX == 12u, "SMS status capacity changed");
_Static_assert(MODEM_SMS_SEGMENT_MAX == 8u, "SMS segment capacity changed");
_Static_assert(MESSAGE_MAILBOX_LIMIT == 500u, "local mailbox limit changed");
_Static_assert(MESSAGE_TEXT_MAX == 2560u, "assembled text bound changed");
_Static_assert(sizeof(message_metadata_t) == 68u, "local metadata RAM budget changed");

_Static_assert(sizeof(modem_sms_message_t) == 472u,
               "modem_sms_message_t layout changed");
_Static_assert(sizeof(((modem_sms_message_t *)0)->text) ==
                   MODEM_SMS_DECODED_TEXT_MAX + 1u,
               "decoded SMS text member does not expose the full UTF-8 bound");
#if defined(__ARM_EABI__)
_Static_assert(sizeof(modem_sms_send_result_t) == 8u,
               "target modem_sms_send_result_t layout changed");
#else
_Static_assert(sizeof(modem_sms_send_result_t) == 12u,
               "host modem_sms_send_result_t layout changed");
#endif

_Static_assert(MODEM_SMS_REQUEST_SEND_TEXT != MODEM_SMS_REQUEST_SEND_BINARY,
               "text and binary sends require distinct request kinds");
_Static_assert(MODEM_SMS_OUTCOME_UNCERTAIN != MODEM_SMS_OUTCOME_ERROR,
               "uncertain delivery must not collapse to retryable failure");

int main(void) {
    printf("test_sms_types: OK\n");
    return 0;
}
