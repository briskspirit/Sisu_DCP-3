#ifndef PHONE_MATCH_H
#define PHONE_MATCH_H

#include <stdbool.h>
#include <stddef.h>

/* Nokia 3210 v6.00 phone-number matching, cloned from the ROM's shared
 * display resolver (core 0x00257fbc -- the one routine behind caller-ID,
 * the SMS list rows, the SMS Sender:/Recipient: pages and the call-register
 * loggers; RE'd 2026-07-06):
 *
 *   - each number reduces to its MATCH FORM: a leading '+' is kept, digits
 *     are kept, and the form TRUNCATES at the first other character (the
 *     original converts BCD nibbles 0xA..0xE -- '*', '#', pause -- to NUL);
 *   - a form of >= 8 chars contributes only its LAST 7 characters; a
 *     shorter form contributes all of itself;
 *   - match = exact string compare of the two contributions.
 *
 * Hence 0501234567 matches +358501234567 (shared 7-char tail), a 7-digit
 * number matches any longer number ending in those digits, and short codes
 * (<= 6 digits) only match exactly. TON/'+' is irrelevant for normal-length
 * numbers because it falls outside the 7-char window. NOTE: the ROM also
 * holds a second, LAST-6 matcher (0x00275938) -- that one serves the SIM
 * phonebook server / send flow, NOT display resolution; this module clones
 * the display rule. */

/* Comfortably above every number source in the firmware (dial editor,
 * phonebook, CLIP, SMS TP-address are all <= ~30 chars); forms are
 * truncated at the cap, which would distort a tail only for inputs that
 * cannot occur. */
#define PHONE_MATCH_FORM_MAX 41u

void phone_match_form(const char *src, char *dst, size_t cap);
bool phone_match_numbers(const char *a, const char *b);

#endif
