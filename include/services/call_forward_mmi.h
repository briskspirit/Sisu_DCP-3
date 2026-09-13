#ifndef CALL_FORWARD_MMI_H
#define CALL_FORWARD_MMI_H

#include "services/call_forward_types.h"

typedef enum {
    CALL_FORWARD_MMI_NOT_MATCHED = 0,
    CALL_FORWARD_MMI_VALID,
    CALL_FORWARD_MMI_INVALID,
} call_forward_mmi_result_t;

/* Parse a complete standby MMI string, including its final '#'. UNKNOWN
 * service codes are not consumed so the ordinary service-code/dial paths can
 * still handle them. Recognized but malformed forwarding codes are consumed
 * as INVALID and must never be dialled as a telephone number. */
call_forward_mmi_result_t call_forward_mmi_parse(
    const char *text, call_forward_request_t *out);

#endif /* CALL_FORWARD_MMI_H */
