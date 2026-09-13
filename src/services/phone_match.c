#include "services/phone_match.h"

#include <string.h>

void phone_match_form(const char *src, char *dst, size_t cap) {
    if (cap == 0u) {
        return;
    }
    size_t out = 0u;
    if (src != 0) {
        for (size_t i = 0u; src[i] != '\0' && out + 1u < cap; i++) {
            char c = src[i];
            if (c >= '0' && c <= '9') {
                dst[out++] = c;
            } else if (c == '+' && i == 0u) {
                dst[out++] = c;
            } else {
                /* First other character ends the form: the original's BCD
                 * nibble map turns '*'/'#'/pause codes into NUL, truncating
                 * the compare string there (ROM nibble table 0x002e1cd0). */
                break;
            }
        }
    }
    dst[out] = '\0';
}

static const char *tail_window(const char *form) {
    size_t len = strlen(form);
    return len >= 8u ? form + (len - 7u) : form;
}

bool phone_match_numbers(const char *a, const char *b) {
    char fa[PHONE_MATCH_FORM_MAX];
    char fb[PHONE_MATCH_FORM_MAX];
    phone_match_form(a, fa, sizeof(fa));
    phone_match_form(b, fb, sizeof(fb));
    if (fa[0] == '\0' || fb[0] == '\0') {
        return false;
    }
    return strcmp(tail_window(fa), tail_window(fb)) == 0;
}
