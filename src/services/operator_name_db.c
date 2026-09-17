#include "services/operator_name_db.h"

#include <stddef.h>
#include <stdint.h>

/* Maintained factual selections, not an imported operator-name database.
 * Sources, exact coverage, and update procedure: docs/operator_names.md. */
#define OPERATOR_DB_NAMES(X) \
    X(a1, "A1") \
    X(airtel, "Airtel") \
    X(att, "AT&T") \
    X(bell, "Bell") \
    X(bouygues, "Bouygues Telecom") \
    X(cell_c, "Cell C") \
    X(cellcom, "Cellcom") \
    X(china_mobile, "China Mobile") \
    X(china_unicom, "China Unicom") \
    X(claro, "Claro") \
    X(digicel, "Digicel") \
    X(dna, "DNA") \
    X(docomo, "NTT DOCOMO") \
    X(drei, "Drei") \
    X(ee, "EE") \
    X(elisa, "Elisa") \
    X(entel, "Entel") \
    X(free_mobile, "Free Mobile") \
    X(globacom, "Globacom") \
    X(ice, "ice") \
    X(kddi, "KDDI") \
    X(kt, "KT") \
    X(kyivstar, "Kyivstar") \
    X(lg_u, "LG U+") \
    X(lifecell, "lifecell") \
    X(m1, "M1") \
    X(magenta, "Magenta") \
    X(mascom, "Mascom") \
    X(movistar, "Movistar") \
    X(mtn, "MTN") \
    X(o2, "O2") \
    X(one_and_one, "1&1") \
    X(one_nz, "One NZ") \
    X(optus, "Optus") \
    X(orange, "Orange") \
    X(partner, "Partner") \
    X(pelephone, "Pelephone") \
    X(rakuten, "Rakuten Mobile") \
    X(rogers, "Rogers") \
    X(safaricom, "Safaricom") \
    X(salt, "Salt") \
    X(sasktel, "SaskTel") \
    X(sfr, "SFR") \
    X(singtel, "Singtel") \
    X(sk_telecom, "SK Telecom") \
    X(softbank, "SoftBank") \
    X(spark, "Spark") \
    X(starhub, "StarHub") \
    X(sunrise, "Sunrise") \
    X(swisscom, "Swisscom") \
    X(t_mobile, "T-Mobile") \
    X(tele2, "Tele2") \
    X(telekom, "Telekom") \
    X(telenor, "Telenor") \
    X(telia, "Telia") \
    X(telkom, "Telkom") \
    X(telstra, "Telstra") \
    X(telus, "TELUS") \
    X(telus_bell, "TELUS/Bell") \
    X(three, "Three") \
    X(tim, "TIM") \
    X(tre, "Tre") \
    X(verizon, "Verizon") \
    X(vivo, "Vivo") \
    X(vodacom, "Vodacom") \
    X(vodafone, "Vodafone")

/* MCC, decimal MNC (never an octal literal), MNC digit count, name ID.
 * Strictly sorted by (MCC, numeric MNC, digit count). Source IDs refer to
 * docs/operator_names.md; B-* sources establish display-brand spellings. */
#define OPERATOR_DB_ROWS(X) \
    /* France: ITU, B-SFR. */ \
    X(208, 1, 2, orange) \
    X(208, 2, 2, orange) \
    X(208, 10, 2, sfr) \
    X(208, 11, 2, sfr) \
    X(208, 15, 2, free_mobile) \
    X(208, 16, 2, free_mobile) \
    X(208, 20, 2, bouygues) \
    X(208, 21, 2, bouygues) \
    /* Spain: DT, B-TEF. */ \
    X(214, 1, 2, vodafone) \
    X(214, 3, 2, orange) \
    X(214, 7, 2, movistar) \
    /* Switzerland: ITU. */ \
    X(228, 1, 2, swisscom) \
    X(228, 2, 2, sunrise) \
    X(228, 3, 2, salt) \
    /* Austria: ITU, B-MAGENTA. */ \
    X(232, 1, 2, a1) \
    X(232, 3, 2, magenta) \
    X(232, 5, 2, drei) \
    /* United Kingdom: ITU, B-O2, B-UK. */ \
    X(234, 10, 2, o2) \
    X(234, 15, 2, vodafone) \
    X(234, 20, 2, three) \
    X(234, 30, 2, ee) \
    X(234, 31, 2, ee) \
    X(234, 32, 2, ee) \
    X(234, 33, 2, ee) \
    X(234, 34, 2, ee) \
    /* Sweden: ITU, B-TRE. */ \
    X(240, 1, 2, telia) \
    X(240, 2, 2, tre) \
    X(240, 6, 2, telenor) \
    X(240, 7, 2, tele2) \
    X(240, 8, 2, telenor) \
    /* Norway: ITU. */ \
    X(242, 1, 2, telenor) \
    X(242, 2, 2, telia) \
    X(242, 14, 2, ice) \
    /* Finland: FI (03/04/05/06/12/13), ITU (21), FI-TELIA (91). */ \
    X(244, 3, 2, dna) \
    X(244, 4, 2, dna) \
    X(244, 5, 2, elisa) \
    X(244, 6, 2, elisa) \
    X(244, 12, 2, dna) \
    X(244, 13, 2, dna) \
    X(244, 21, 2, elisa) \
    X(244, 91, 2, telia) \
    /* Ukraine: ITU, B-UA. */ \
    X(255, 1, 2, vodafone) \
    X(255, 2, 2, kyivstar) \
    X(255, 3, 2, kyivstar) \
    X(255, 6, 2, lifecell) \
    /* Germany: DE, B-TEF, B-1AND1. */ \
    X(262, 1, 2, telekom) \
    X(262, 2, 2, vodafone) \
    X(262, 3, 2, o2) \
    X(262, 7, 2, o2) \
    X(262, 23, 2, one_and_one) \
    /* Canada: CA. Keep the shared allocation neutral. */ \
    X(302, 220, 3, telus) \
    X(302, 250, 3, bell) \
    X(302, 610, 3, bell) \
    X(302, 640, 3, bell) \
    X(302, 680, 3, sasktel) \
    X(302, 720, 3, rogers) \
    X(302, 880, 3, telus_bell) \
    /* United States: US. 010/012/016 retain three digits. */ \
    X(310, 10, 3, verizon) \
    X(310, 12, 3, verizon) \
    X(310, 16, 3, att) \
    X(310, 120, 3, t_mobile) \
    X(310, 150, 3, att) \
    X(310, 170, 3, att) \
    X(310, 240, 3, t_mobile) \
    X(310, 260, 3, t_mobile) \
    X(310, 410, 3, att) \
    X(311, 480, 3, verizon) \
    /* Jamaica: ITU. */ \
    X(338, 50, 3, digicel) \
    /* India: ITU; selected Airtel circles only, not a national range. */ \
    X(404, 2, 2, airtel) \
    X(404, 3, 2, airtel) \
    X(404, 6, 2, airtel) \
    X(404, 10, 2, airtel) \
    /* Israel: ITU. */ \
    X(425, 1, 2, partner) \
    X(425, 2, 2, cellcom) \
    X(425, 3, 2, pelephone) \
    /* Japan: ITU. */ \
    X(440, 0, 2, softbank) \
    X(440, 10, 2, docomo) \
    X(440, 11, 2, rakuten) \
    X(440, 20, 2, softbank) \
    X(440, 50, 2, kddi) \
    X(440, 51, 2, kddi) \
    /* South Korea: ITU. */ \
    X(450, 5, 2, sk_telecom) \
    X(450, 6, 2, lg_u) \
    X(450, 8, 2, kt) \
    /* China: ITU; no inference from obsolete CDMA allocation labels. */ \
    X(460, 0, 2, china_mobile) \
    X(460, 1, 2, china_unicom) \
    /* Australia: ITU. */ \
    X(505, 1, 2, telstra) \
    X(505, 2, 2, optus) \
    X(505, 3, 2, vodafone) \
    /* Singapore: ITU. */ \
    X(525, 1, 2, singtel) \
    X(525, 3, 2, m1) \
    X(525, 5, 2, starhub) \
    /* New Zealand: ITU (01), NZ-SPARK (05). */ \
    X(530, 1, 2, one_nz) \
    X(530, 5, 2, spark) \
    /* Fiji: ITU. */ \
    X(542, 1, 2, vodafone) \
    X(542, 2, 2, digicel) \
    /* Nigeria: ITU. */ \
    X(621, 30, 2, mtn) \
    X(621, 50, 2, globacom) \
    /* Kenya: ITU. */ \
    X(639, 1, 2, safaricom) \
    X(639, 2, 2, safaricom) \
    X(639, 3, 2, airtel) \
    X(639, 7, 2, telkom) \
    /* Botswana: ITU. */ \
    X(652, 1, 2, mascom) \
    X(652, 2, 2, orange) \
    /* South Africa: ITU. */ \
    X(655, 1, 2, vodacom) \
    X(655, 2, 2, telkom) \
    X(655, 7, 2, cell_c) \
    X(655, 10, 2, mtn) \
    /* Brazil: ITU, B-TEF. */ \
    X(724, 2, 2, tim) \
    X(724, 3, 2, tim) \
    X(724, 4, 2, tim) \
    X(724, 5, 2, claro) \
    X(724, 6, 2, vivo) \
    X(724, 10, 2, vivo) \
    X(724, 11, 2, vivo) \
    X(724, 23, 2, vivo) \
    /* Chile: ITU, B-TEF. */ \
    X(730, 1, 2, entel) \
    X(730, 2, 2, movistar) \
    X(730, 7, 2, movistar) \
    X(730, 10, 2, entel)

/* An all-char struct gives compiler-computed string offsets without a
 * pointer table, relocation per row, manual offsets, or padded name slots. */
#define NAME_FIELD(id, text) char id[sizeof(text)];
typedef struct {
    OPERATOR_DB_NAMES(NAME_FIELD)
} operator_name_pool_t;
#undef NAME_FIELD

#define NAME_VALUE(id, text) .id = text,
static const operator_name_pool_t s_operator_names = {
    OPERATOR_DB_NAMES(NAME_VALUE)
};
#undef NAME_VALUE

#define NAME_CHECK(id, text) \
    _Static_assert(sizeof(text) > 1u && \
                   sizeof(text) <= OPERATOR_NAME_DB_NAME_MAX + 1u, \
                   "operator name must contain 1..16 bytes: " #id);
OPERATOR_DB_NAMES(NAME_CHECK)
#undef NAME_CHECK

/* 21 key bits + 11 string-offset bits = one uint32_t per PLMN. */
#define NAME_OFFSET_BITS 11u
#define NAME_OFFSET_LIMIT (UINT32_C(1) << NAME_OFFSET_BITS)
#define NAME_OFFSET_MASK (NAME_OFFSET_LIMIT - 1u)
#define PLMN_KEY(mcc, mnc, digits) \
    (((uint32_t)(mcc) * 1000u + (uint32_t)(mnc)) * 2u + \
     (uint32_t)(digits) - 2u)

_Static_assert(sizeof(s_operator_names) <= NAME_OFFSET_LIMIT,
               "operator string pool exceeds packed offset capacity");

#define ROW_CHECK(mcc, mnc, digits, name) \
    _Static_assert((mcc) >= 0 && (mcc) <= 999 && (mnc) >= 0 && \
                   (((digits) == 2 && (mnc) <= 99) || \
                    ((digits) == 3 && (mnc) <= 999)), \
                   "invalid PLMN: " #mcc "/" #mnc "/" #digits);
OPERATOR_DB_ROWS(ROW_CHECK)
#undef ROW_CHECK

#define ROW_VALUE(mcc, mnc, digits, name) \
    ((PLMN_KEY(mcc, mnc, digits) << NAME_OFFSET_BITS) | \
     (uint32_t)offsetof(operator_name_pool_t, name)),
static const uint32_t s_operator_rows[] = {
    OPERATOR_DB_ROWS(ROW_VALUE)
};
#undef ROW_VALUE

const char *operator_name_db_lookup(const char *mcc, const char *mnc) {
    if (mcc == NULL || mnc == NULL) {
        return NULL;
    }

    uint32_t country = 0u;
    for (size_t i = 0u; i < 3u; i++) {
        if (mcc[i] < '0' || mcc[i] > '9') {
            return NULL;
        }
        country = country * 10u + (uint32_t)(mcc[i] - '0');
    }
    if (mcc[3] != '\0') {
        return NULL;
    }

    uint32_t network = 0u;
    size_t digits = 0u;
    while (digits < 3u && mnc[digits] != '\0') {
        if (mnc[digits] < '0' || mnc[digits] > '9') {
            return NULL;
        }
        network = network * 10u + (uint32_t)(mnc[digits] - '0');
        digits++;
    }
    if (digits < 2u || mnc[digits] != '\0') {
        return NULL;
    }

    uint32_t key = PLMN_KEY(country, network, digits);
    size_t low = 0u;
    size_t high = sizeof(s_operator_rows) / sizeof(s_operator_rows[0]);
    while (low < high) {
        size_t mid = low + (high - low) / 2u;
        uint32_t row = s_operator_rows[mid];
        uint32_t row_key = row >> NAME_OFFSET_BITS;
        if (key < row_key) {
            high = mid;
        } else if (key > row_key) {
            low = mid + 1u;
        } else {
            return (const char *)&s_operator_names + (row & NAME_OFFSET_MASK);
        }
    }
    return NULL;
}
