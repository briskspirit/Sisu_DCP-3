#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "services/strings.h"
#include "generated/strings_data.h"

static int s_failures;

static void assert_true(bool condition, const char *message) {
    if (!condition) {
        fprintf(stderr, "FAIL: %s\n", message);
        s_failures++;
    }
}

static void assert_ts(uint16_t sid, const char *expect, const char *message) {
    const char *got = ts(sid);
    if (got == 0 || strcmp(got, expect) != 0) {
        fprintf(stderr, "FAIL: %s -- ts(0x%03x)=\"%s\" expected \"%s\"\n",
                message, sid, got ? got : "(null)", expect);
        s_failures++;
    }
}

static void assert_softkey(const char *caption, const char *expect, const char *message) {
    const char *got = ts_softkey(caption);
    if (got == 0 || strcmp(got, expect) != 0) {
        fprintf(stderr, "FAIL: %s -- ts_softkey(\"%s\")=\"%s\" expected \"%s\"\n",
                message, caption, got ? got : "(null)", expect);
        s_failures++;
    }
}

/* Oracle: expected values are the v6.00 per-language PPM records read directly
 * from the RE tables. ts(sid) resolves record index (sid - 58) in the active
 * language; "Phone book" is SID 0x2ce (index 660) -> GERM "Verzeichnis". */

static void test_ts_sid(void) {
    strings_set_language(1u);
    assert_true(strings_get_language() == 1u, "active language is 1");
    assert_ts(0x18bu, "Serial No.\n%S",
              "EN SID 0x18b = *#06# serial template");
    assert_ts(0x2ceu, "Phone book", "EN SID 0x2ce = Phone book");
    assert_ts(0x337u, "Message", "EN SID 0x337 = recipient-less Outbox label");
    assert_ts(0x33au,
              "No number\nfound\non this screen",
              "EN SID 0x33a = Use number empty-result note");
    assert_ts(0x139u, "New e-mail\nmessage",
              "EN SID 0x139 = singular e-mail MWI notice");
    assert_ts(0x13au, "%S\nnew e-mail\nmessages",
              "EN SID 0x13a = plural e-mail MWI notice");
    assert_ts(0x146u, "New fax\nmessage",
              "EN SID 0x146 = singular fax MWI notice");
    assert_ts(0x147u, "%S\nnew fax\nmessages",
              "EN SID 0x147 = plural fax MWI notice");
    /* Translation spot checks only run when those languages are compiled in
     * (the set is a build choice; English above is the guaranteed minimum). */
    bool have_german = false, have_french = false;
    for (uint8_t i = 0u; i < strings_language_count(); i++) {
        have_german |= strings_language_id_at(i) == 2u;
        have_french |= strings_language_id_at(i) == 3u;
    }
    if (have_german) {
        strings_set_language(2u);
        assert_ts(0x2ceu, "Verzeichnis", "DE SID 0x2ce = Verzeichnis");
        assert_ts(0x06eu, "Kurz-\nmitteilungen", "DE SID 0x06e = Messages");
    }
    if (have_french) {
        strings_set_language(3u);
        assert_ts(0x098u, "Journal", "FR SID 0x098 = Call register");
    }
    strings_set_language(1u);
    assert_true(ts(0u) == 0, "sid 0 -> NULL");
    assert_true(ts(57u) == 0, "sid below base -> NULL");
    assert_true(ts(60000u) == 0, "sid above range -> NULL");
}

static void test_ts_softkey(void) {
    /* Framework menu-bar block: Back=0x2d8, OK=0x2e9, Options=0x2ea,
     * Select=0x2ee. In German those localize; non-softkey captions pass through. */
    strings_set_language(2u);
    assert_softkey("Back", ts(0x2d8u), "DE softkey Back = framework 0x2d8");
    assert_softkey("Options", ts(0x2eau), "DE softkey Options = framework 0x2ea");
    assert_softkey("Select", ts(0x2eeu), "DE softkey Select = framework 0x2ee");
    /* A caption that is not a framework softkey passes through unchanged. */
    assert_softkey("zzz not a softkey", "zzz not a softkey", "non-softkey passes through");
    strings_set_language(1u);
    assert_softkey("OK", "OK", "EN softkey OK stays OK");
}

static void test_table_integrity(void) {
    /* Contract, not configuration: the compiled set is a build choice, but it
     * is never empty and always contains English (the guaranteed minimum). */
    assert_true(g_string_table_count >= 1u, "at least one language compiled");
    bool has_english = false;
    for (uint8_t i = 0u; i < g_string_table_count; i++) {
        assert_true(g_string_tables[i].records != 0, "table has records");
        if (g_string_tables[i].lang_id == 1u) {
            has_english = true;
        }
    }
    assert_true(has_english, "English table present");
}

static void test_language_registry(void) {
    assert_true(strings_language_count() == g_string_table_count,
                "registry count mirrors the generated table");
    assert_true(strings_language_id_at(0u) == 1u,
                "English is always first (the guaranteed minimum)");
    assert_true(strcmp(strings_language_self_name_at(0u), "English") == 0,
                "English self-name");
    /* Order is the generator's menu order (the traced v6.00 sequence), NOT a
     * numeric id sort -- only uniqueness and English-first are contractual. */
    for (uint8_t i = 0u; i < strings_language_count(); i++) {
        const char *name = strings_language_self_name_at(i);
        assert_true(name != 0 && name[0] != '\0', "every language has a self-name");
        for (uint8_t j = (uint8_t)(i + 1u); j < strings_language_count(); j++) {
            assert_true(strings_language_id_at(i) != strings_language_id_at(j),
                        "language ids are unique");
        }
    }
    assert_true(strings_language_id_at(255u) == 1u,
                "out-of-range registry index clamps to English");
}

static void test_traced_menu_order(void) {
    /* Ground truth: the v6.00c selector lists its pack's TEXT chunks in pack
     * order -- ENGL GERM FREN GREE BULG HUNG ROMA POLI CZEC SLVA CROA SERB
     * SLVE RUSS ESTO LATV LITH. Whenever the full shipped set is compiled
     * (any selection containing it), those 17 must open the registry in
     * exactly that order; a numeric-id sort or a shuffled generator table
     * fails here. Partial custom sets skip the check (subset order is still
     * covered by the generator's own test_assetgen suite). */
    static const uint8_t traced[17] = {
        1u, 2u, 3u, 11u, 19u, 12u, 29u, 28u, 21u,
        31u, 20u, 30u, 32u, 15u, 24u, 26u, 27u,
    };
    uint8_t present = 0u;
    for (uint8_t k = 0u; k < 17u; k++) {
        for (uint8_t i = 0u; i < strings_language_count(); i++) {
            if (strings_language_id_at(i) == traced[k]) {
                present++;
                break;
            }
        }
    }
    if (present != 17u) {
        return; /* custom subset build: golden order not applicable */
    }
    for (uint8_t k = 0u; k < 17u; k++) {
        if (strings_language_id_at(k) != traced[k]) {
            fprintf(stderr, "FAIL: traced menu order -- index %u has lang id "
                    "%u, the v6.00 selector has %u\n",
                    k, strings_language_id_at(k), traced[k]);
            s_failures++;
        }
    }
}

int main(void) {
    test_ts_sid();
    test_ts_softkey();
    test_table_integrity();
    test_language_registry();
    test_traced_menu_order();
    if (s_failures != 0) {
        fprintf(stderr, "%d assertion(s) failed\n", s_failures);
        return 1;
    }
    printf("test_strings: all assertions passed\n");
    return 0;
}
