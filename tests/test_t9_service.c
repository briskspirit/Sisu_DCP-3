#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "generated/t9_ldb.h"
#include "services/t9_service.h"

static int s_failures;

static void assert_true(bool condition, const char *message) {
    if (!condition) {
        fprintf(stderr, "FAIL: %s\n", message);
        s_failures++;
    }
}

static void assert_eq_int(long got, long want, const char *message) {
    if (got != want) {
        fprintf(stderr, "FAIL: %s (got %ld want %ld)\n", message, got, want);
        s_failures++;
    }
}

static void assert_eq_str(const char *got, const char *want, const char *message) {
    if (strcmp(got, want) != 0) {
        fprintf(stderr, "FAIL: %s (got \"%s\" want \"%s\")\n", message, got, want);
        s_failures++;
    }
}

/* ------------------------------------------------------------------ */
/* Helpers to validate candidate-list invariants                      */
/* ------------------------------------------------------------------ */

/* Independent reimplementation of the digit-class mapping (the spec for
 * a phone keypad). Returns the T9 digit ('2'..'9') a lowercase letter
 * belongs to, or '\0' for any non-letter. */
static char expected_digit_for_letter(char ch) {
    static const char *groups[] = {
        "abc", "def", "ghi", "jkl", "mno", "pqrs", "tuv", "wxyz"};
    for (int g = 0; g < 8; g++) {
        for (const char *p = groups[g]; *p; p++) {
            if (*p == ch) {
                return (char)('2' + g);
            }
        }
    }
    return '\0';
}

/* Assert every candidate is non-empty, NUL-terminated within bounds, has no
 * duplicate, and matches the requested key sequence digit-for-digit. */
static void check_candidate_list(const t9_candidate_list_t *list,
                                 const char *sequence,
                                 const char *what) {
    assert_true(list->count <= T9_CANDIDATE_LIMIT, what);
    size_t seqlen = strlen(sequence);
    for (uint8_t i = 0u; i < list->count; i++) {
        const char *w = list->words[i];
        /* NUL terminator must exist within the fixed slot. */
        size_t wl = strnlen(w, T9_WORD_MAX + 1u);
        assert_true(wl <= T9_WORD_MAX, what);
        assert_true(wl > 0u, what);
        /* Each candidate must match the key sequence: same length, each
         * letter mapping back to the correct digit. */
        assert_eq_int((long)wl, (long)seqlen, "candidate length matches sequence length");
        for (size_t j = 0u; j < wl && j < seqlen; j++) {
            char lc = w[j];
            if (lc >= 'A' && lc <= 'Z') {
                lc = (char)(lc + ('a' - 'A'));
            }
            char d = expected_digit_for_letter(lc);
            assert_true(d == sequence[j], "candidate letter maps to sequence digit");
        }
        /* No duplicates. */
        for (uint8_t k = (uint8_t)(i + 1u); k < list->count; k++) {
            assert_true(strcmp(w, list->words[k]) != 0, "no duplicate candidates");
        }
    }
}

/* ------------------------------------------------------------------ */
/* dictionary metadata                                                 */
/* ------------------------------------------------------------------ */

static void test_dictionary_metadata(void) {
    /* Contract, not configuration: the service mirrors whatever registry was
     * generated, English always first. */
    uint8_t n = t9_dictionary_count();
    assert_eq_int(n, g_t9_ldb_count, "service mirrors the generated registry");
    assert_true(n >= 1u, "at least one dictionary compiled");

    const t9_dictionary_info_t *engl = t9_dictionary_info(0u);
    assert_eq_str(engl->tag, "ENGL", "dict0 tag");
    assert_eq_str(engl->label, "English", "dict0 label");
    assert_eq_int(t9_dictionary_lang_id(0u), 1, "dict0 is language id 1");

    for (uint8_t i = 0u; i < n; i++) {
        const t9_dictionary_info_t *info = t9_dictionary_info(i);
        assert_eq_str(info->tag, g_t9_ldbs[i].tag, "info tag mirrors registry");
        assert_eq_str(info->label, g_t9_ldbs[i].label, "info label mirrors registry");
        assert_eq_int(t9_dictionary_index_for_tag(info->tag), i,
                      "tag round-trips to its index");
        assert_eq_int(t9_dictionary_index_for_lang_id(t9_dictionary_lang_id(i)), i,
                      "lang id round-trips to its index");
    }

    /* Out-of-range index clamps to 0. */
    const t9_dictionary_info_t *oob = t9_dictionary_info(99u);
    assert_eq_str(oob->tag, "ENGL", "oob index clamps to dict0");

    assert_eq_int(t9_dictionary_index_for_tag("nope"), 0, "unknown tag -> 0");
    assert_eq_int(t9_dictionary_index_for_tag(NULL), 0, "null tag -> 0");
    assert_eq_int(t9_dictionary_index_for_lang_id(0u), 0, "unknown lang id -> 0");
}

/* The registry's layout parameters are parsed from each LDB's own header at
 * generation time. Lock the derivation against the historically hand-derived
 * values for the original trio so a generator regression cannot ship. */
static void test_registry_derived_params(void) {
    assert_true(g_t9_ldb_count >= 1u, "at least one dictionary compiled");
    assert_eq_str(g_t9_ldbs[0].tag, "ENGL", "English dictionary always first");

    static const struct {
        const char *tag;
        uint16_t dictionary_id, char_table, trie_root;
        uint16_t shorts[4];
        uint16_t blk_base, pay_base;
        uint16_t thresholds[2];
    } EXPECTED[] = {
        {"ENGL", 0x0009u, 0x005cu, 0x031cu,
         {0x0358u, 0x0371u, 0x0373u, 0x0386u}, 0x03a3u, 0x0ba3u, {0x0040u, 0u}},
        {"GER", 0x0007u, 0x005cu, 0x031cu,
         {0x035du, 0x037cu, 0x039au, 0x03bau}, 0x03d6u, 0x0c16u, {0x003eu, 0x0002u}},
        {"FREN", 0x000cu, 0x005cu, 0x031cu,
         {0x036fu, 0x038au, 0x03a1u, 0x03b4u}, 0x03c9u, 0x0bc9u, {0x0040u, 0u}},
    };
    for (uint8_t e = 0u; e < 3u; e++) {
        for (uint8_t i = 0u; i < g_t9_ldb_count; i++) {
            const t9_ldb_desc_t *d = &g_t9_ldbs[i];
            if (strcmp(d->tag, EXPECTED[e].tag) != 0) {
                continue;
            }
            assert_true(d->dictionary_id == EXPECTED[e].dictionary_id &&
                            d->char_table_offset == EXPECTED[e].char_table &&
                            d->trie_root_offset == EXPECTED[e].trie_root &&
                            d->block_pointer_base_offset == EXPECTED[e].blk_base &&
                            d->payload_base_offset == EXPECTED[e].pay_base,
                        "derived scalar params match the hand-derived set");
            for (uint8_t j = 0u; j < 4u; j++) {
                assert_true(d->short_pointer_offsets[j] == EXPECTED[e].shorts[j],
                            "derived short-pointer offsets match");
            }
            assert_true(d->block_pointer_thresholds[0] == EXPECTED[e].thresholds[0] &&
                            d->block_pointer_thresholds[1] == EXPECTED[e].thresholds[1],
                        "derived thresholds match");
        }
    }
}

/* ------------------------------------------------------------------ */
/* candidate lookup: well-formed sequences                            */
/* ------------------------------------------------------------------ */

/* Confirm a candidate set contains a given word. */
static bool list_contains(const t9_candidate_list_t *list, const char *word) {
    for (uint8_t i = 0u; i < list->count; i++) {
        /* compare case-insensitively */
        const char *a = list->words[i];
        const char *b = word;
        bool same = true;
        size_t la = strlen(a), lb = strlen(b);
        if (la != lb) {
            continue;
        }
        for (size_t j = 0u; j < la; j++) {
            char ca = a[j], cb = b[j];
            if (ca >= 'A' && ca <= 'Z') ca = (char)(ca + 32);
            if (cb >= 'A' && cb <= 'Z') cb = (char)(cb + 32);
            if (ca != cb) { same = false; break; }
        }
        if (same) {
            return true;
        }
    }
    return false;
}

static void test_known_words(void) {
    t9_candidate_list_t list;

#if defined(SISU_SYNTHETIC_ASSETS)
    /* The public fresh-clone fixture deliberately contains no proprietary word
     * trie. Keep the service's empty-dictionary behavior under ASan while the
     * real-asset run below remains the Nokia/Tegic data-fidelity oracle. */
    assert_true(!t9_candidates_for_sequence("43556", 0u, &list),
                "synthetic dictionary is intentionally empty");
    assert_eq_int(list.count, 0, "synthetic no-match count is zero");
    return;
#endif

    /* "4663" -> classic T9 demo: home/good/gone/hood ... at least one of
     * these common English words must appear. */
    assert_true(t9_candidates_for_sequence("4663", 0u, &list), "4663 yields candidates");
    check_candidate_list(&list, "4663", "4663 candidates valid");
    assert_true(list.count > 0u, "4663 has at least one candidate");
    assert_true(list_contains(&list, "good") || list_contains(&list, "home") ||
                    list_contains(&list, "gone") || list_contains(&list, "hood"),
                "4663 includes a common 4663 word");

    /* "43556" -> "hello" is the canonical example. */
    assert_true(t9_candidates_for_sequence("43556", 0u, &list), "43556 yields candidates");
    check_candidate_list(&list, "43556", "43556 candidates valid");
    assert_true(list_contains(&list, "hello"), "43556 includes hello");

    /* "63" -> "of"/"me"/"ne"... a 2-letter sequence. */
    assert_true(t9_candidates_for_sequence("63", 0u, &list), "63 yields candidates");
    check_candidate_list(&list, "63", "63 candidates valid");

    /* single key "2" -> "a"/"c"/"b" single letters. */
    assert_true(t9_candidates_for_sequence("2", 0u, &list), "single key 2 yields candidates");
    check_candidate_list(&list, "2", "single key 2 valid");
    for (uint8_t i = 0u; i < list.count; i++) {
        assert_eq_int((long)strlen(list.words[i]), 1, "single-key candidates are length 1");
    }
}

static void test_cycling_candidates_distinct_and_valid(void) {
    /* The candidate list IS the cycle order the UI walks through; every
     * entry must be distinct and valid for the sequence. */
    t9_candidate_list_t list;
    const char *seqs[] = {"2", "22", "222", "2222", "8", "84", "843", "8433",
                          "726", "7378", "96753", "4663", "228", "63"};
    for (size_t s = 0; s < sizeof(seqs) / sizeof(seqs[0]); s++) {
        memset(&list, 0xAB, sizeof(list)); /* poison */
        bool ok = t9_candidates_for_sequence(seqs[s], 0u, &list);
        /* whether or not there is a match, count and words must be coherent */
        assert_true(list.count <= T9_CANDIDATE_LIMIT, "count bounded after lookup");
        if (ok) {
            assert_true(list.count > 0u, "ok implies count>0");
            check_candidate_list(&list, seqs[s], "cycle candidates valid");
        } else {
            assert_eq_int(list.count, 0, "no-match implies count 0");
        }
    }
}

/* ------------------------------------------------------------------ */
/* candidate lookup: invalid / edge sequences                         */
/* ------------------------------------------------------------------ */

static void test_invalid_sequences_rejected(void) {
    t9_candidate_list_t list;

    /* NULL out pointer */
    assert_true(!t9_candidates_for_sequence("4663", 0u, NULL), "null out rejected");

    /* NULL sequence */
    assert_true(!t9_candidates_for_sequence(NULL, 0u, &list), "null sequence rejected");
    assert_eq_int(list.count, 0, "null sequence count 0");

    /* empty sequence */
    assert_true(!t9_candidates_for_sequence("", 0u, &list), "empty sequence rejected");
    assert_eq_int(list.count, 0, "empty sequence count 0");

    /* 0-key and 1-key are not letter keys -> rejected */
    assert_true(!t9_candidates_for_sequence("0", 0u, &list), "0-key rejected");
    assert_true(!t9_candidates_for_sequence("1", 0u, &list), "1-key rejected");
    assert_true(!t9_candidates_for_sequence("4660", 0u, &list), "embedded 0 rejected");
    assert_true(!t9_candidates_for_sequence("4661", 0u, &list), "embedded 1 rejected");

    /* punctuation / non-digit characters */
    assert_true(!t9_candidates_for_sequence("46*3", 0u, &list), "asterisk rejected");
    assert_true(!t9_candidates_for_sequence("46#3", 0u, &list), "hash rejected");
    assert_true(!t9_candidates_for_sequence("abc", 0u, &list), "letters rejected");
    assert_true(!t9_candidates_for_sequence(" 466", 0u, &list), "leading space rejected");
}

static void test_no_match_sequence(void) {
    t9_candidate_list_t list;
    /* A long valid digit sequence that is extremely unlikely to spell any
     * dictionary word -> must return false with a clean empty list and no
     * out-of-bounds reads (ASan watches). */
    const char *gibberish = "2222222222"; /* 10 keys, all class 2 */
    memset(&list, 0x7F, sizeof(list));
    bool ok = t9_candidates_for_sequence(gibberish, 0u, &list);
    if (!ok) {
        assert_eq_int(list.count, 0, "gibberish no-match count 0");
    } else {
        check_candidate_list(&list, gibberish, "gibberish (matched) valid");
    }
}

static void test_max_length_sequence(void) {
    t9_candidate_list_t list;

    /* Exactly T9_SEQUENCE_MAX (32) digits: accepted by is_t9_sequence
     * (index 31 passes the i>=32 check) and must not overrun classes[32]. */
    char seq32[T9_SEQUENCE_MAX + 1u];
    for (uint8_t i = 0u; i < T9_SEQUENCE_MAX; i++) {
        seq32[i] = (char)('2' + (i % 8));
    }
    seq32[T9_SEQUENCE_MAX] = '\0';
    memset(&list, 0, sizeof(list));
    (void)t9_candidates_for_sequence(seq32, 0u, &list); /* result is data-dependent; just must not crash */
    if (list.count > 0u) {
        check_candidate_list(&list, seq32, "32-key candidates valid");
    }

    /* One over the max (33 digits): must be rejected by is_t9_sequence at
     * i==32 (i>=T9_SEQUENCE_MAX) before any class buffer is touched. */
    char seq33[T9_SEQUENCE_MAX + 2u];
    for (uint8_t i = 0u; i < T9_SEQUENCE_MAX + 1u; i++) {
        seq33[i] = (char)('2' + (i % 8));
    }
    seq33[T9_SEQUENCE_MAX + 1u] = '\0';
    memset(&list, 0, sizeof(list));
    assert_true(!t9_candidates_for_sequence(seq33, 0u, &list), "33-key sequence rejected");
    assert_eq_int(list.count, 0, "33-key count 0");

    /* Way over max */
    char big[200];
    for (int i = 0; i < 199; i++) {
        big[i] = (char)('2' + (i % 8));
    }
    big[199] = '\0';
    assert_true(!t9_candidates_for_sequence(big, 0u, &list), "200-key sequence rejected");
}

static void test_all_dictionaries(void) {
    /* Run a battery of sequences against every dictionary; every returned
     * candidate must respect the buffer + key-sequence invariants (ASan +
     * the manual checks catch corruption in any language's LDB walk). */
    const char *seqs[] = {"2", "3", "4", "5", "6", "7", "8", "9",
                          "43556", "4663", "726", "8378", "63", "228",
                          "7777", "999", "23456789", "2468", "97539"};
    for (uint8_t d = 0u; d < t9_dictionary_count(); d++) {
        for (size_t s = 0; s < sizeof(seqs) / sizeof(seqs[0]); s++) {
            t9_candidate_list_t list;
            memset(&list, 0xCD, sizeof(list));
            bool ok = t9_candidates_for_sequence(seqs[s], d, &list);
            assert_true(list.count <= T9_CANDIDATE_LIMIT, "dict count bounded");
            if (ok) {
                check_candidate_list(&list, seqs[s], "dict candidate valid");
            } else {
                assert_eq_int(list.count, 0, "dict no-match count 0");
            }
        }
    }

    /* Out-of-range dictionary index clamps to 0 (must still produce valid
     * candidates, not read OOB). */
    t9_candidate_list_t list;
    bool ok = t9_candidates_for_sequence("43556", 250u, &list);
    if (ok) {
        check_candidate_list(&list, "43556", "oob-dict clamps and stays valid");
    }
}

/* ------------------------------------------------------------------ */
/* t9_fallback_word                                                    */
/* ------------------------------------------------------------------ */

static void test_fallback_word(void) {
    char buf[64];

    /* Each digit maps to one representative letter per the code's table. */
    t9_fallback_word("23456789", buf, sizeof(buf));
    assert_eq_str(buf, "aeilnsty", "fallback maps each digit to its letter");

    /* 0 and 1 and unknown chars are dropped (default: break, no append). */
    t9_fallback_word("2013", buf, sizeof(buf));
    assert_eq_str(buf, "ae", "fallback drops 0 and 1");

    /* empty input -> empty string */
    t9_fallback_word("", buf, sizeof(buf));
    assert_eq_str(buf, "", "fallback empty input");

    /* NULL sequence -> empty, no crash */
    buf[0] = 'X';
    t9_fallback_word(NULL, buf, sizeof(buf));
    assert_eq_str(buf, "", "fallback null sequence");

    /* cap == 0 -> no write at all (must not touch buf) */
    char guard[4] = {'A', 'B', 'C', 'D'};
    t9_fallback_word("2222", guard, 0u);
    assert_true(guard[0] == 'A', "fallback cap 0 writes nothing");

    /* cap == 1 -> only the NUL fits */
    char one[1];
    one[0] = 'Z';
    t9_fallback_word("2222", one, 1u);
    assert_eq_int(one[0], '\0', "fallback cap 1 writes only NUL");

    /* tight cap truncates and always NUL-terminates within cap. Use cap=4 on a
     * larger buffer so byte[4] is a guard that must stay untouched. */
    char small[8];
    memset(small, 0x55, sizeof(small));
    t9_fallback_word("2345678", small, 4u); /* would be "aeilnst" */
    assert_true(strlen(small) <= 3u, "fallback respects cap");
    assert_eq_int(small[4], 0x55, "fallback never writes past cap (guard byte intact)");
    assert_eq_str(small, "aei", "fallback truncates to cap-1");
}

/* ------------------------------------------------------------------ */
/* t9_display_word: case shifting                                      */
/* ------------------------------------------------------------------ */

static void test_display_word(void) {
    char buf[64];

    /* mode 0 (lowercase), sentence_start=false -> all lowercase */
    t9_display_word("Hello", 0u, false, buf, sizeof(buf));
    assert_eq_str(buf, "hello", "mode0 non-sentence lowercases all");

    /* mode 0, sentence_start=true -> capitalize first only */
    t9_display_word("hello", 0u, true, buf, sizeof(buf));
    assert_eq_str(buf, "Hello", "mode0 sentence-start caps first only");

    /* mode 1 (mixed/title?) -> per code, non-mode0/2 falls to lowercase */
    t9_display_word("HELLO", 1u, false, buf, sizeof(buf));
    assert_eq_str(buf, "hello", "mode1 lowercases (not all-caps)");

    /* mode 2 -> ALL CAPS */
    t9_display_word("hello", 2u, false, buf, sizeof(buf));
    assert_eq_str(buf, "HELLO", "mode2 uppercases all");
    t9_display_word("HeLLo", 2u, true, buf, sizeof(buf));
    assert_eq_str(buf, "HELLO", "mode2 ignores sentence_start, all caps");

    /* mode 0 sentence_start but first char not a letter -> unchanged first */
    t9_display_word("123ab", 0u, true, buf, sizeof(buf));
    assert_eq_str(buf, "123ab", "mode0 sentence-start non-letter first stays, rest lower");

    /* empty word -> empty */
    t9_display_word("", 2u, false, buf, sizeof(buf));
    assert_eq_str(buf, "", "display empty word");

    /* NULL word -> empty, no crash */
    buf[0] = 'Q';
    t9_display_word(NULL, 0u, false, buf, sizeof(buf));
    assert_eq_str(buf, "", "display null word");

    /* cap 0 -> no write */
    char guard[3] = {'A', 'B', 'C'};
    t9_display_word("hi", 2u, false, guard, 0u);
    assert_eq_int(guard[0], 'A', "display cap 0 writes nothing");

    /* tight cap truncates + NUL-terminates, no overrun (cap=4, guard at [4]) */
    char small[8];
    memset(small, 0x33, sizeof(small));
    t9_display_word("hello", 2u, false, small, 4u);
    assert_eq_str(small, "HEL", "display truncates to cap-1");
    assert_eq_int(small[4], 0x33, "display never writes past cap");

    /* non-letters pass through in each mode */
    t9_display_word("a1!z", 2u, false, buf, sizeof(buf));
    assert_eq_str(buf, "A1!Z", "display mode2 keeps non-letters");
}

/* ------------------------------------------------------------------ */
/* t9_signature: word -> key sequence                                  */
/* ------------------------------------------------------------------ */

static void test_signature(void) {
    char buf[64];

    /* Round-trip: signature of a known word must equal its key sequence. */
    assert_true(t9_signature("hello", buf, sizeof(buf)), "signature hello ok");
    assert_eq_str(buf, "43556", "signature hello = 43556");

    assert_true(t9_signature("Good", buf, sizeof(buf)), "signature Good ok (case-insensitive)");
    assert_eq_str(buf, "4663", "signature Good = 4663");

    /* Independent oracle over a-z. */
    for (char c = 'a'; c <= 'z'; c++) {
        char w[2] = {c, '\0'};
        char sig[4];
        bool ok = t9_signature(w, sig, sizeof(sig));
        char exp = expected_digit_for_letter(c);
        assert_true(ok, "single letter signature ok");
        char want[2] = {exp, '\0'};
        assert_eq_str(sig, want, "single letter signature matches keypad");
        /* uppercase form yields same signature */
        char W[2] = {(char)(c - 32), '\0'};
        char sig2[4];
        assert_true(t9_signature(W, sig2, sizeof(sig2)), "upper letter signature ok");
        assert_eq_str(sig2, want, "upper letter signature matches keypad");
    }

    /* Non-letters are skipped, not encoded. */
    assert_true(t9_signature("a1b!c", buf, sizeof(buf)), "signature with punctuation ok");
    assert_eq_str(buf, "222", "signature skips non-letters -> abc=222");

    /* A word with no letters at all -> returns false, empty string. */
    buf[0] = 'X';
    assert_true(!t9_signature("12 34", buf, sizeof(buf)), "no-letter word -> false");
    assert_eq_str(buf, "", "no-letter signature empties buffer");

    /* empty word -> false, empty */
    assert_true(!t9_signature("", buf, sizeof(buf)), "empty word -> false");
    assert_eq_str(buf, "", "empty signature");

    /* NULL word -> false (loop skipped), buffer NUL'd, no crash */
    buf[0] = 'Y';
    assert_true(!t9_signature(NULL, buf, sizeof(buf)), "null word -> false");
    assert_eq_str(buf, "", "null signature empties buffer");

    /* cap 0 -> false, no write */
    char guard[2] = {'A', 'B'};
    assert_true(!t9_signature("abc", guard, 0u), "signature cap 0 -> false");
    assert_eq_int(guard[0], 'A', "signature cap 0 writes nothing");

    /* tight cap truncates + NUL-terminates within cap, no overrun (cap=4). */
    char small[8];
    memset(small, 0x22, sizeof(small));
    bool ok = t9_signature("hello", small, 4u); /* "43556" -> truncate */
    assert_true(ok, "signature truncated still ok");
    assert_eq_str(small, "435", "signature truncates to cap-1");
    assert_eq_int(small[4], 0x22, "signature never writes past cap");
}

/* ------------------------------------------------------------------ */
/* round-trip: candidate -> signature == original sequence            */
/* ------------------------------------------------------------------ */

static void test_candidate_signature_round_trip(void) {
    const char *seqs[] = {"4663", "43556", "726", "2", "63", "8378", "228"};
    for (size_t s = 0; s < sizeof(seqs) / sizeof(seqs[0]); s++) {
        t9_candidate_list_t list;
        if (!t9_candidates_for_sequence(seqs[s], 0u, &list)) {
            continue;
        }
        for (uint8_t i = 0u; i < list.count; i++) {
            char sig[T9_WORD_MAX + 1u];
            bool ok = t9_signature(list.words[i], sig, sizeof(sig));
            assert_true(ok, "candidate has a signature");
            assert_eq_str(sig, seqs[s], "candidate signature == original sequence");
        }
    }
}

int main(void) {
    test_dictionary_metadata();
    test_registry_derived_params();
    test_known_words();
    test_cycling_candidates_distinct_and_valid();
    test_invalid_sequences_rejected();
    test_no_match_sequence();
    test_max_length_sequence();
    test_all_dictionaries();
    test_fallback_word();
    test_display_word();
    test_signature();
    test_candidate_signature_round_trip();

    if (s_failures != 0) {
        fprintf(stderr, "%d failures\n", s_failures);
        return 1;
    }
    printf("t9_service tests passed\n");
    return 0;
}
