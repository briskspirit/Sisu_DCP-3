#ifndef SERVICES_STRINGS_H
#define SERVICES_STRINGS_H

#include <stdint.h>

/* Project-owned SIDs, outside the original ROM's namespace. */
enum {
    SID_LOCAL_PHONE_MEMORY = 0x8000,
    SID_LOCAL_KIB_FREE,
    SID_LOCAL_CONTACT_COUNT,
    SID_LOCAL_STORAGE_UNAVAILABLE,
    SID_DATA_MESSAGE,
};

/* UI localization by v6.00 string id (SID), exactly like the ROM: each UI site
 * names its exact SID via ts()/ts_or(); softkeys resolve centrally via
 * ts_softkey(). User/dynamic content has no SID and is never localized. Only
 * LTR languages are localized; Hebrew/Arabic remain English pending a bidi/
 * shaping pass. */

void strings_set_language(uint8_t lang_id);
uint8_t strings_get_language(void);

/* Localize by v6.00 string id (the faithful path: each UI site names its exact
 * SID, exactly like the ROM, so context-specific translations resolve
 * correctly and user/dynamic content -- which has no SID -- is never touched).
 * Returns the active language's string for `sid`, English when that language
 * leaves it untranslated, or NULL for a sid with no v6.00 record (clone-only
 * strings, e.g. Net Monitor) so the caller can fall back to its own literal. */
const char *ts(uint16_t sid);

/* Convenience for UI sites: ts(sid) with an explicit English fallback literal
 * (kept inline as both documentation and the string shown for a clone-only
 * sid). This is the standard form for localized labels: ts_or(0x2ce, "Phone
 * book"). */
const char *ts_or(uint16_t sid, const char *fallback);

/* Localize a fixed softkey caption via the v6.00 framework "menu-bar" string
 * block (the exact SIDs the ROM menu bar uses). Used centrally by draw_softkey;
 * returns the caption unchanged if it is not a framework softkey. */
const char *ts_softkey(const char *caption);

/* Compiled-language registry (mirrors the generated g_string_tables). Index
 * order is the generator's emission order — the faithful v6.00 menu sequence
 * (the original selector lists the PPM pack's TEXT chunks in pack order),
 * English always first. Menus list exactly this set; self-names are native
 * script and are never translated. */
uint8_t strings_language_count(void);
uint8_t strings_language_id_at(uint8_t index);
const char *strings_language_self_name_at(uint8_t index);

#endif /* SERVICES_STRINGS_H */
