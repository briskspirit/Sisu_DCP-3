#include "services/strings.h"

#include <string.h>

#include "generated/strings_data.h"

/* English is g_string_tables[0] (the generator emits it first): the fallback
 * for any untranslated record. s_records is the active language's table. */
static uint8_t s_lang_id = 1u;
static const char *const *s_records = 0; /* active table; set on first use */

static const char *const *records_for(uint8_t lang_id) {
    for (uint8_t i = 0u; i < g_string_table_count; i++) {
        if (g_string_tables[i].lang_id == lang_id) {
            return g_string_tables[i].records;
        }
    }
    return g_string_tables[0].records; /* unknown id -> English */
}

void strings_set_language(uint8_t lang_id) {
    s_lang_id = lang_id;
    s_records = records_for(lang_id);
}

uint8_t strings_get_language(void) {
    return s_lang_id;
}

/* SID-based lookup (the faithful path -- matches how v6.00 references every UI
 * string by id). Returns the active language's record for `sid`, falling back
 * to the English record when that language leaves it untranslated. An
 * out-of-range sid (e.g. a clone-only string with no v6.00 id, like Net
 * Monitor) returns NULL so the caller can substitute its own literal. */
const char *ts(uint16_t sid) {
    /* English fallback for project-owned records. Translations can be added
     * here independently of the immutable ROM tables. Numeric formats take
     * one unsigned long argument. */
    switch (sid) {
    case SID_LOCAL_PHONE_MEMORY: return "Phone:";
    case SID_LOCAL_KIB_FREE: return "%lu KiB free";
    case SID_LOCAL_CONTACT_COUNT: return "%lu contacts";
    case SID_LOCAL_STORAGE_UNAVAILABLE: return "Unavailable";
    case SID_DATA_MESSAGE: return "Data message";
    default: break;
    }
    if (sid < STRINGS_SID_BASE) {
        return 0;
    }
    uint16_t idx = (uint16_t)(sid - STRINGS_SID_BASE);
    if (idx >= STRINGS_RECORD_COUNT) {
        return 0;
    }
    const char *const *active = (s_records != 0) ? s_records : g_string_tables[0].records;
    const char *t = active[idx];
    if (t == 0 || t[0] == '\0') {
        t = g_string_tables[0].records[idx]; /* English fallback */
    }
    return (t != 0 && t[0] != '\0') ? t : 0;
}

const char *ts_or(uint16_t sid, const char *fallback) {
    const char *t = ts(sid);
    return (t != 0) ? t : fallback;
}

uint8_t strings_language_count(void) {
    return g_string_table_count;
}

uint8_t strings_language_id_at(uint8_t index) {
    if (index >= g_string_table_count) {
        index = 0u;
    }
    return g_string_tables[index].lang_id;
}

const char *strings_language_self_name_at(uint8_t index) {
    if (index >= g_string_table_count) {
        index = 0u;
    }
    return g_string_tables[index].self_name;
}

const char *ts_softkey(const char *caption) {
    /* v6.00 draws menu/dialog softkeys from a contiguous framework "menu-bar"
     * string block (SIDs 0x2d8..0x2f8: Back/OK/Options/Select/Read/Save/Send/
     * Snooze/Stop/View/Yes/...). The clone's draw_softkey is that same framework
     * path, so resolve a fixed English caption to its exact framework SID and
     * localize it -- this is why softkeys are NOT localized by generic text
     * lookup (the same word maps to different SIDs per subsystem; the framework
     * block is the one the menu bar uses). A caption already resolved to a
     * context-specific SID by an app (an already-localized, non-English string)
     * won't match here and is returned unchanged. */
    if (caption == 0 || caption[0] == '\0') {
        return caption;
    }
    const char *const *eng = g_string_tables[0].records; /* English */
    /* The menu-bar caption block runs 0x2d6..0x2f8: 0x2d6 "Answer" and 0x2d7
     * "Assign" sit just below "Back" (0x2d8), so the lower bound must reach them
     * or the incoming-call "Answer" softkey never localizes. */
    for (uint16_t sid = 0x2d6u; sid <= 0x2f8u; sid++) {
        uint16_t idx = (uint16_t)(sid - STRINGS_SID_BASE);
        if (idx < STRINGS_RECORD_COUNT && eng[idx] != 0 && strcmp(eng[idx], caption) == 0) {
            return ts(sid);
        }
    }
    return caption;
}
