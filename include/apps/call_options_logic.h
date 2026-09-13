#ifndef CALL_OPTIONS_LOGIC_H
#define CALL_OPTIONS_LOGIC_H

#include <stdbool.h>
#include <stdint.h>

/* In-call Options item bits, 1:1 with the v6.00 template table. The ROM emits
 * set bits in ascending order; calls_app maps these bits to localized labels. */
enum {
    CALL_OPT_HOLD       = 0x001u,
    CALL_OPT_UNHOLD     = 0x002u,
    CALL_OPT_NEW_CALL   = 0x004u,
    CALL_OPT_END_THIS   = 0x008u,
    CALL_OPT_SWAP       = 0x010u,
    CALL_OPT_ANSWER     = 0x020u,
    CALL_OPT_REJECT     = 0x040u,
    CALL_OPT_SEND_DTMF  = 0x080u,
    /* 0x100 "Send" belongs to the in-call number editor, not this menu. */
    CALL_OPT_END_ALL    = 0x200u,
    CALL_OPT_PHONE_BOOK = 0x400u,
};

/* Return the v6.00 call-options mask for the visible call topology, then
 * remove the backend hold toggle when it cannot safely target one sole leg.
 * Answer/Reject/Swap remain available for their explicit multi-leg actions. */
uint16_t call_options_mask_for_state(bool active, bool held, bool waiting,
                                     bool hold_toggle_available);

#endif /* CALL_OPTIONS_LOGIC_H */
