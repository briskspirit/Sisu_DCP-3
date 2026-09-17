#include <stdbool.h>
#include <stdio.h>
#include <string.h>

/* Single TU, as in other focused service tests: inspect the private table
 * without adding a firmware enumeration API or test-only firmware state.
 * Compile this file alone; do not also link operator_name_db.c. */
#include "../src/services/operator_name_db.c"

static int s_failures;

static void check(bool condition, const char *label) {
    if (!condition) {
        fprintf(stderr, "FAIL: %s\n", label);
        s_failures++;
    }
}

static void expect_name(const char *mcc, const char *mnc, const char *expected) {
    const char *actual = operator_name_db_lookup(mcc, mnc);
    if (actual == NULL || strcmp(actual, expected) != 0) {
        fprintf(stderr, "FAIL: %s/%s: expected %s, got %s\n",
                mcc, mnc, expected, actual == NULL ? "NULL" : actual);
        s_failures++;
    }
}

static void test_known_names(void) {
    /* Independent fixtures, including both table endpoints, each region,
     * rebrands, shared allocations, zero MNC and leading-zero 3-digit MNCs. */
    static const struct {
        const char *mcc;
        const char *mnc;
        const char *name;
    } fixtures[] = {
        {"208", "01", "Orange"},
        {"208", "10", "SFR"},
        {"208", "15", "Free Mobile"},
        {"208", "20", "Bouygues Telecom"},
        {"214", "07", "Movistar"},
        {"228", "01", "Swisscom"},
        {"232", "03", "Magenta"},
        {"234", "10", "O2"},
        {"234", "15", "Vodafone"},
        {"234", "20", "Three"},
        {"234", "30", "EE"},
        {"234", "33", "EE"},
        {"240", "02", "Tre"},
        {"240", "07", "Tele2"},
        {"242", "01", "Telenor"},
        {"242", "14", "ice"},
        {"244", "03", "DNA"},
        {"244", "05", "Elisa"},
        {"244", "21", "Elisa"},
        {"244", "91", "Telia"},
        {"255", "01", "Vodafone"},
        {"255", "03", "Kyivstar"},
        {"255", "06", "lifecell"},
        {"262", "01", "Telekom"},
        {"262", "03", "O2"},
        {"262", "23", "1&1"},
        {"302", "220", "TELUS"},
        {"302", "610", "Bell"},
        {"302", "720", "Rogers"},
        {"302", "880", "TELUS/Bell"},
        {"310", "010", "Verizon"},
        {"310", "012", "Verizon"},
        {"310", "016", "AT&T"},
        {"310", "120", "T-Mobile"},
        {"310", "260", "T-Mobile"},
        {"310", "410", "AT&T"},
        {"311", "480", "Verizon"},
        {"338", "050", "Digicel"},
        {"404", "10", "Airtel"},
        {"425", "03", "Pelephone"},
        {"440", "00", "SoftBank"},
        {"440", "10", "NTT DOCOMO"},
        {"440", "11", "Rakuten Mobile"},
        {"440", "50", "KDDI"},
        {"450", "05", "SK Telecom"},
        {"450", "06", "LG U+"},
        {"460", "00", "China Mobile"},
        {"460", "01", "China Unicom"},
        {"505", "01", "Telstra"},
        {"505", "02", "Optus"},
        {"525", "01", "Singtel"},
        {"525", "05", "StarHub"},
        {"530", "01", "One NZ"},
        {"530", "05", "Spark"},
        {"542", "02", "Digicel"},
        {"621", "30", "MTN"},
        {"639", "02", "Safaricom"},
        {"652", "01", "Mascom"},
        {"655", "01", "Vodacom"},
        {"655", "07", "Cell C"},
        {"724", "05", "Claro"},
        {"724", "10", "Vivo"},
        {"730", "10", "Entel"},
    };
    for (size_t i = 0u; i < sizeof(fixtures) / sizeof(fixtures[0]); i++) {
        expect_name(fixtures[i].mcc, fixtures[i].mnc, fixtures[i].name);
    }
}

static void test_unknown_and_width(void) {
    static const char *const misses[][2] = {
        {"001", "01"}, {"001", "001"}, {"000", "00"},
        {"999", "99"}, {"999", "999"}, {"901", "01"},
        {"208", "00"}, {"208", "22"}, {"730", "11"},
        {"244", "99"}, {"244", "07"}, {"244", "36"},
        {"310", "999"}, {"310", "035"}, {"310", "000"},
        {"244", "005"}, {"244", "050"}, {"244", "5"},
        {"310", "10"}, {"310", "12"}, {"310", "16"},
        {"310", "26"}, {"310", "41"}, {"311", "48"},
        {"338", "50"}, {"440", "000"}, {"460", "000"},
        {"460", "001"}, {"724", "010"},
    };
    for (size_t i = 0u; i < sizeof(misses) / sizeof(misses[0]); i++) {
        if (operator_name_db_lookup(misses[i][0], misses[i][1]) != NULL) {
            fprintf(stderr, "FAIL: unexpected hit for %s/%s\n",
                    misses[i][0], misses[i][1]);
            s_failures++;
        }
    }
}

static void test_malformed(void) {
    static const char *const bad_mcc[] = {
        NULL, "", "3", "31", "0310", "3100", "310410",
        "+310", "-310", " 310", "310 ", "31A", "31\n", "3.0",
        "310,", "310/", "\"310\"", "\xff" "10", "310xxxxxxxxxx",
    };
    static const char *const bad_mnc[] = {
        NULL, "", "4", "0410", "4100", "310410", "+41", "-41",
        " 410", "410 ", "41A", "41\n", "4.0", "41,", "41/",
        "\"410\"", "\xff" "10", "410xxxxxxxxxx",
    };
    for (size_t i = 0u; i < sizeof(bad_mcc) / sizeof(bad_mcc[0]); i++) {
        check(operator_name_db_lookup(bad_mcc[i], "410") == NULL,
              "malformed MCC is rejected");
    }
    for (size_t i = 0u; i < sizeof(bad_mnc) / sizeof(bad_mnc[0]); i++) {
        check(operator_name_db_lookup("310", bad_mnc[i]) == NULL,
              "malformed MNC is rejected");
    }
    check(operator_name_db_lookup(NULL, NULL) == NULL, "both arguments NULL");

    for (unsigned int byte = 0u; byte <= 255u; byte++) {
        if (byte >= (unsigned int)'0' && byte <= (unsigned int)'9') {
            continue;
        }
        for (size_t pos = 0u; pos < 3u; pos++) {
            char mcc[] = "310";
            char mnc[] = "410";
            mcc[pos] = (char)byte;
            mnc[pos] = (char)byte;
            check(operator_name_db_lookup(mcc, "410") == NULL,
                  "every non-ASCII-digit MCC byte is rejected");
            check(operator_name_db_lookup("310", mnc) == NULL,
                  "non-digit MNC byte cannot produce an AT&T hit");
        }
    }

    /* Exactly sized objects exercise short-input bounds under ASan. */
    const char empty[1] = {'\0'};
    const char short_mcc[2] = {'3', '\0'};
    const char short_mnc[2] = {'4', '\0'};
    check(operator_name_db_lookup(empty, "410") == NULL, "empty MCC bounds");
    check(operator_name_db_lookup(short_mcc, "410") == NULL, "short MCC bounds");
    check(operator_name_db_lookup("310", empty) == NULL, "empty MNC bounds");
    check(operator_name_db_lookup("310", short_mnc) == NULL, "short MNC bounds");
}

typedef struct {
    unsigned int mcc;
    unsigned int mnc;
    unsigned int digits;
    const char *name;
} unpacked_row_t;

/* Unpacked oracle: bypass the production bit packing and binary search. */
#define UNPACKED_ROW(mcc, mnc, digits, name) \
    {mcc, mnc, digits, s_operator_names.name},
static const unpacked_row_t s_unpacked[] = {
    OPERATOR_DB_ROWS(UNPACKED_ROW)
};
#undef UNPACKED_ROW

static bool before(const unpacked_row_t *a, const unpacked_row_t *b) {
    if (a->mcc != b->mcc) {
        return a->mcc < b->mcc;
    }
    if (a->mnc != b->mnc) {
        return a->mnc < b->mnc;
    }
    return a->digits < b->digits;
}

static void test_database_invariants(void) {
    const size_t count = sizeof(s_operator_rows) / sizeof(s_operator_rows[0]);
    const char *pool = (const char *)&s_operator_names;
    check(count == 120u, "coverage count matches documented snapshot");
    check(count == sizeof(s_unpacked) / sizeof(s_unpacked[0]),
          "packed and unpacked row counts agree");
    check(sizeof(s_operator_rows[0]) == 4u, "exactly four bytes per record");

    for (size_t i = 0u; i < count; i++) {
        const unpacked_row_t *expected = &s_unpacked[i];
        uint32_t row = s_operator_rows[i];
        uint32_t key = row >> NAME_OFFSET_BITS;
        size_t offset = (size_t)(row & NAME_OFFSET_MASK);
        if (i != 0u) {
            check(before(&s_unpacked[i - 1u], expected),
                  "source rows strictly sorted with no duplicate PLMN");
            check((s_operator_rows[i - 1u] >> NAME_OFFSET_BITS) < key,
                  "packed keys strictly sorted");
        }
        check(key / 2u / 1000u == expected->mcc &&
              key / 2u % 1000u == expected->mnc &&
              key % 2u + 2u == expected->digits,
              "packing preserves MCC, MNC, and MNC width");
        check(offset < sizeof(s_operator_names), "name offset within pool");
        if (offset >= sizeof(s_operator_names)) {
            continue;
        }
        check(offset == 0u || pool[offset - 1u] == '\0',
              "name offset points to a whole string, not a suffix");
        check(pool + offset == expected->name, "packed offset selects intended name");
    }

    size_t name_count = 0u;
    for (size_t offset = 0u; offset < sizeof(s_operator_names);) {
        const char *name = pool + offset;
        const char *end = memchr(name, '\0', sizeof(s_operator_names) - offset);
        check(end != NULL, "every pool string terminates within pool");
        if (end == NULL) {
            break;
        }
        size_t len = (size_t)(end - name);
        check(len > 0u && len <= OPERATOR_NAME_DB_NAME_MAX,
              "names contain 1..16 characters with no pool padding");
        check(name[0] != ' ' && (len == 0u || name[len - 1u] != ' '),
              "no leading or trailing name whitespace");
        for (size_t i = 0u; i < len; i++) {
            check(name[i] >= 0x20 && name[i] <= 0x7e, "printable ASCII names only");
        }
        bool used = false;
        for (size_t i = 0u; i < count; i++) {
            used = used || s_unpacked[i].name == name;
        }
        check(used, "no unreferenced names waste flash");
        for (size_t prior = 0u; prior < offset;) {
            check(strcmp(pool + prior, name) != 0, "pool strings deduplicated");
            prior += strlen(pool + prior) + 1u;
        }
        offset += len + 1u;
        name_count++;
    }
    check(name_count == 66u, "unique-name count matches snapshot");
    printf("Database: %zu rows, %zu names, %zu row bytes + %zu name bytes = %zu bytes\n",
           count, name_count, sizeof(s_operator_rows), sizeof(s_operator_names),
           sizeof(s_operator_rows) + sizeof(s_operator_names));
}

static void test_complete_input_space(void) {
    const size_t count = sizeof(s_unpacked) / sizeof(s_unpacked[0]);
    size_t next = 0u;
    size_t hits = 0u;
    size_t queries = 0u;
    for (unsigned int mcc = 0u; mcc <= 999u; mcc++) {
        char country[4];
        (void)snprintf(country, sizeof(country), "%03u", mcc);
        for (unsigned int mnc = 0u; mnc <= 999u; mnc++) {
            for (unsigned int digits = 2u; digits <= 3u; digits++) {
                if (digits == 2u && mnc > 99u) {
                    continue;
                }
                char network[4];
                (void)snprintf(network, sizeof(network), "%0*u", (int)digits, mnc);
                const char *expected = NULL;
                if (next < count && s_unpacked[next].mcc == mcc &&
                    s_unpacked[next].mnc == mnc && s_unpacked[next].digits == digits) {
                    expected = s_unpacked[next++].name;
                    hits++;
                }
                const char *actual = operator_name_db_lookup(country, network);
                if (actual != expected) {
                    fprintf(stderr, "FAIL: exhaustive lookup mismatch at %s/%s\n",
                            country, network);
                    s_failures++;
                    return;
                }
                queries++;
            }
        }
    }
    check(next == count && hits == count, "every intended PLMN reached exactly once");
    check(queries == 1100000u, "all 5- and 6-digit numeric PLMNs checked");
    printf("Exhaustive lookup: %zu queries, %zu hits\n", queries, hits);
}

static void test_purity_and_lifetime(void) {
    char mcc[] = "310";
    char mnc[] = "410";
    const char *att = operator_name_db_lookup(mcc, mnc);
    check(strcmp(mcc, "310") == 0 && strcmp(mnc, "410") == 0,
          "lookup does not mutate inputs");
    mcc[0] = '9';
    mnc[0] = '9';
    expect_name("311", "480", "Verizon");
    check(att != NULL && strcmp(att, "AT&T") == 0,
          "result survives other calls and changes to input buffers");
    check(att == operator_name_db_lookup("310", "410"), "stable result pointer");
    check(operator_name_db_lookup("234", "15") ==
          operator_name_db_lookup("505", "03"), "shared brands reuse one string");
    check(strlen(s_operator_names.bouygues) == OPERATOR_NAME_DB_NAME_MAX,
          "16-character name is not truncated");
}

int main(void) {
    test_known_names();
    test_unknown_and_width();
    test_malformed();
    test_database_invariants();
    test_complete_input_space();
    test_purity_and_lifetime();
    if (s_failures != 0) {
        return 1;
    }
    puts("PASS: operator name database");
    return 0;
}
