#include "modem_vendor_telit_internal.h"

#include <stdio.h>
#include <string.h>

/* ------------------------------------------------------------------------ */
/* Automatic antenna-tuner policy and strict provisioning readback.         */
/* ------------------------------------------------------------------------ */

/* #STUNEANT Rev. 4 defines bits 0..34. The measured policy below covers
 * every encodable band supplied by the RF owner. LTE B6/B10/B17/B27/B30,
 * B65/B68/B85 have no #STUNEANT bit and therefore cannot be programmed by
 * this LE910Cx command. WCDMA B6 is distinct and is encoded in RF3. */
/* GPIO2 is CTRL1/ant1_cfg; GPIO3 is CTRL2/ant2_cfg. */
static const uint8_t TELIT_TUNE_CTRL1[TELIT_TUNE_RF_COUNT] = {0u, 0u, 1u, 1u};
static const uint8_t TELIT_TUNE_CTRL2[TELIT_TUNE_RF_COUNT] = {0u, 1u, 0u, 1u};

/* LTE: B1/B2/B3/B9/B25; WCDMA: B1/B2; GSM: DCS1800/PCS1900. */
#define TELIT_TUNE_RF1_POLICY UINT64_C(0x60184087)
/* LTE: B12/B13/B28/B71. */
#define TELIT_TUNE_RF2_POLICY UINT64_C(0x00050300)
/* LTE: B5/B8/B14/B18/B19/B20/B26; WCDMA: B5/B6/B8/B19;
 * GSM: 850/E-GSM900. */
#define TELIT_TUNE_RF3_POLICY UINT64_C(0x1F80BC50)
/* LTE: B4/B66; WCDMA: B4. */
#define TELIT_TUNE_RF4_POLICY UINT64_C(0x00420008)

_Static_assert(((TELIT_TUNE_RF1_POLICY & TELIT_TUNE_RF2_POLICY) |
                (TELIT_TUNE_RF1_POLICY & TELIT_TUNE_RF3_POLICY) |
                (TELIT_TUNE_RF1_POLICY & TELIT_TUNE_RF4_POLICY) |
                (TELIT_TUNE_RF2_POLICY & TELIT_TUNE_RF3_POLICY) |
                (TELIT_TUNE_RF2_POLICY & TELIT_TUNE_RF4_POLICY) |
                (TELIT_TUNE_RF3_POLICY & TELIT_TUNE_RF4_POLICY)) == 0u,
               "Telit tuner policy masks must remain disjoint");
_Static_assert(((TELIT_TUNE_RF1_POLICY | TELIT_TUNE_RF2_POLICY |
                 TELIT_TUNE_RF3_POLICY | TELIT_TUNE_RF4_POLICY) &
                ~TELIT_TUNE_COMMAND_DOMAIN_MASK) == 0u,
               "Telit tuner policy exceeds #STUNEANT mask domain");

static const uint64_t TELIT_TUNE_POLICY[TELIT_TUNE_RF_COUNT] = {
    TELIT_TUNE_RF1_POLICY,
    TELIT_TUNE_RF2_POLICY,
    TELIT_TUNE_RF3_POLICY,
    TELIT_TUNE_RF4_POLICY,
};

typedef struct {
    bool saw_row;
    bool zero_row;
    bool invalid;
    uint64_t state_mask[TELIT_TUNE_RF_COUNT];
    uint64_t union_mask;
} telit_tune_readback_t;

static uint64_t s_telit_tune_supported_mask;
static bool s_telit_tune_supported_valid;
static bool s_telit_tune_enabled;
static bool s_telit_tune_repair_needed;
static telit_tune_rf_t s_telit_tune_readback_target;
static telit_tune_readback_t s_telit_tune_readback;

uint64_t telit_tune_policy_mask(telit_tune_rf_t rf) {
    return rf < TELIT_TUNE_RF_COUNT ? TELIT_TUNE_POLICY[rf] : 0u;
}

uint64_t telit_tune_policy_union(void) {
    uint64_t mask = 0u;
    for (size_t i = 0u; i < TELIT_TUNE_RF_COUNT; i++) {
        mask |= TELIT_TUNE_POLICY[i];
    }
    return mask;
}

bool telit_tune_controls(telit_tune_rf_t rf, bool *ctrl1, bool *ctrl2) {
    if (rf >= TELIT_TUNE_RF_COUNT || ctrl1 == NULL || ctrl2 == NULL) {
        return false;
    }
    *ctrl1 = TELIT_TUNE_CTRL1[rf] != 0u;
    *ctrl2 = TELIT_TUNE_CTRL2[rf] != 0u;
    return true;
}

uint64_t telit_tune_effective_mask(telit_tune_rf_t rf) {
    if (!s_telit_tune_supported_valid || rf >= TELIT_TUNE_RF_COUNT) {
        return 0u;
    }
    uint64_t mask =
        s_telit_tune_supported_mask & TELIT_TUNE_POLICY[rf];
    if (rf == TELIT_TUNE_RF1) {
        /* Telit always drives one of the four states. Unmeasured supported
         * bands (WWX LTE B7, and any future domain bit) therefore use the
         * electrically deterministic 00/RF1 fallback rather than inheriting
         * a stale NVM row. */
        mask |= s_telit_tune_supported_mask & ~telit_tune_policy_union();
    }
    return mask;
}

bool telit_init_parse_stune_capability(const char *line) {
    static const char prefix[] = "#STUNEANT: (0,1),(";
    static const char suffix[] = "),(0,1),(0,1)";
    if (!telit_starts_with(line, prefix)) {
        return false;
    }
    size_t line_len = strlen(line);
    size_t prefix_len = sizeof(prefix) - 1u;
    size_t suffix_len = sizeof(suffix) - 1u;
    if (line_len <= prefix_len + suffix_len ||
        memcmp(line + line_len - suffix_len, suffix, suffix_len) != 0) {
        return false;
    }
    size_t mask_len = line_len - prefix_len - suffix_len;
    if (mask_len > UINT8_MAX) {
        return false;
    }
    telit_csv_view_t mask_field = {
        .text = line + prefix_len,
        .length = (uint8_t)mask_len,
    };
    uint64_t mask = 0u;
    if (!telit_view_parse_hex_u64(mask_field,
                                  TELIT_TUNE_COMMAND_DOMAIN_MASK, &mask) ||
        mask == 0u) {
        return false;
    }
    s_telit_tune_supported_mask = mask;
    s_telit_tune_supported_valid = true;
    s_telit_tune_enabled = false;
    s_telit_tune_repair_needed = true;
    return true;
}

modem_provision_line_t telit_provision_stune_discover(
    const char *line) {
    if (!telit_starts_with(line, "#STUNEANT:")) {
        return MODEM_PROVISION_LINE_IGNORE;
    }
    telit_csv_view_t fields[1];
    size_t count = 0u;
    uint32_t enabled = 0u;
    if (!telit_view_split_prefixed(line, "#STUNEANT:", fields, 1u,
                                   &count) ||
        count != 1u || !telit_view_parse_u32(fields[0], 1u, &enabled)) {
        return MODEM_PROVISION_LINE_INVALID;
    }
    s_telit_tune_enabled = enabled != 0u;
    s_telit_tune_repair_needed = !s_telit_tune_enabled;
    return MODEM_PROVISION_LINE_MATCH;
}

modem_provision_line_t telit_provision_stune_disabled(
    const char *line) {
    if (!telit_starts_with(line, "#STUNEANT:")) {
        return MODEM_PROVISION_LINE_IGNORE;
    }
    telit_csv_view_t fields[1];
    size_t count = 0u;
    uint32_t enabled = 0u;
    if (!telit_view_split_prefixed(line, "#STUNEANT:", fields, 1u,
                                   &count) ||
        count != 1u || !telit_view_parse_u32(fields[0], 1u, &enabled)) {
        return MODEM_PROVISION_LINE_INVALID;
    }
    if (enabled == 0u) {
        s_telit_tune_enabled = false;
        return MODEM_PROVISION_LINE_MATCH;
    }
    return MODEM_PROVISION_LINE_MISMATCH;
}

void telit_tune_readback_begin(void) {
    memset(&s_telit_tune_readback, 0, sizeof(s_telit_tune_readback));
}

telit_tune_rf_t telit_tune_rf_from_controls(uint8_t ctrl1,
                                                   uint8_t ctrl2) {
    if (ctrl1 == 0u) {
        return ctrl2 == 0u ? TELIT_TUNE_RF1 : TELIT_TUNE_RF2;
    }
    return ctrl2 == 0u ? TELIT_TUNE_RF3 : TELIT_TUNE_RF4;
}

modem_provision_line_t telit_provision_gtune_line(
    const char *line) {
    if (!telit_starts_with(line, "#GTUNEANT:")) {
        return MODEM_PROVISION_LINE_IGNORE;
    }
    telit_csv_view_t fields[3];
    size_t count = 0u;
    uint64_t mask = 0u;
    uint32_t ctrl1 = 0u;
    uint32_t ctrl2 = 0u;
    if (!s_telit_tune_supported_valid ||
        !telit_view_split_prefixed(line, "#GTUNEANT:", fields, 3u,
                                   &count) ||
        count != 3u ||
        !telit_view_parse_hex_u64(fields[0],
                                  TELIT_TUNE_COMMAND_DOMAIN_MASK, &mask) ||
        !telit_view_parse_u32(fields[1], 1u, &ctrl1) ||
        !telit_view_parse_u32(fields[2], 1u, &ctrl2) ||
        (mask & ~s_telit_tune_supported_mask) != 0u ||
        (mask & s_telit_tune_readback.union_mask) != 0u) {
        s_telit_tune_readback.invalid = true;
        return MODEM_PROVISION_LINE_INVALID;
    }

    if (mask == 0u) {
        /* WWX can expose zero-mask placeholder rows after interrupted or
         * incomplete provisioning. They carry no assignment and are safe to
         * reconstruct, unlike overlap or out-of-domain corruption. */
        s_telit_tune_readback.zero_row = true;
        s_telit_tune_readback.saw_row = true;
        return MODEM_PROVISION_LINE_IGNORE;
    }

    telit_tune_rf_t rf = telit_tune_rf_from_controls(
        (uint8_t)ctrl1, (uint8_t)ctrl2);
    s_telit_tune_readback.state_mask[rf] |= mask;
    s_telit_tune_readback.union_mask |= mask;
    s_telit_tune_readback.saw_row = true;
    return MODEM_PROVISION_LINE_IGNORE;
}

static bool telit_tune_readback_structurally_valid(void) {
    return s_telit_tune_supported_valid &&
           s_telit_tune_readback.saw_row &&
           !s_telit_tune_readback.zero_row &&
           !s_telit_tune_readback.invalid &&
           s_telit_tune_readback.union_mask == s_telit_tune_supported_mask;
}

bool telit_tune_readback_exact(void) {
    if (!telit_tune_readback_structurally_valid()) {
        return false;
    }
    for (telit_tune_rf_t rf = TELIT_TUNE_RF1;
         rf < TELIT_TUNE_RF_COUNT; rf++) {
        if (s_telit_tune_readback.state_mask[rf] !=
            telit_tune_effective_mask(rf)) {
            return false;
        }
    }
    return true;
}

modem_provision_line_t telit_tune_discovery_finish(
    bool command_ok, bool timed_out) {
    if (timed_out || !s_telit_tune_supported_valid ||
        s_telit_tune_readback.invalid ||
        (!command_ok && s_telit_tune_readback.saw_row)) {
        return MODEM_PROVISION_LINE_INVALID;
    }
    /* A carrier factory restore can leave STUNEANT enabled while GTUNEANT
     * returns an error without rows. Rebuild under CFUN=4; the final exact
     * readback still gates RF, including when the error persists. */
    /* A syntactically valid but incomplete table can result from an
     * interrupted earlier write. Treat it as repairable policy mismatch;
     * overlapping/out-of-domain rows remain invalid and fail closed. */
    s_telit_tune_repair_needed = !telit_tune_readback_exact();
    return MODEM_PROVISION_LINE_MATCH;
}

modem_provision_line_t telit_tune_row_finish(bool command_ok,
                                                     bool timed_out) {
    if (timed_out || !s_telit_tune_supported_valid ||
        s_telit_tune_readback.invalid) {
        return MODEM_PROVISION_LINE_INVALID;
    }
    if (!command_ok) {
        return s_telit_tune_readback.saw_row
                   ? MODEM_PROVISION_LINE_INVALID
                   : MODEM_PROVISION_LINE_MISMATCH;
    }
    uint64_t target_mask =
        telit_tune_effective_mask(s_telit_tune_readback_target);
    if (target_mask == 0u) {
        return MODEM_PROVISION_LINE_INVALID;
    }
    /* #STUNEANT rewrites one state at a time. During that sequence WWX
     * legitimately reports zero-mask placeholders plus a shrinking default
     * 00 row, so the table is not globally complete after the first writes.
     * Each incremental verify owns only its target row; the final query below
     * remains responsible for exact, exhaustive table validation. */
    return (s_telit_tune_readback.state_mask[s_telit_tune_readback_target] &
            target_mask) == target_mask
               ? MODEM_PROVISION_LINE_MATCH
               : MODEM_PROVISION_LINE_MISMATCH;
}

modem_provision_line_t telit_tune_final_finish(bool command_ok,
                                                       bool timed_out) {
    if (timed_out || !command_ok ||
        !telit_tune_readback_structurally_valid()) {
        return MODEM_PROVISION_LINE_INVALID;
    }
    if (!telit_tune_readback_exact()) {
        return MODEM_PROVISION_LINE_MISMATCH;
    }
    s_telit_tune_enabled = true;
    s_telit_tune_repair_needed = false;
    return MODEM_PROVISION_LINE_MATCH;
}

bool telit_tune_table_discovery_applicable(void) {
    return s_telit_tune_supported_valid && s_telit_tune_enabled;
}

bool telit_tune_repair_applicable(void) {
    return s_telit_tune_supported_valid && s_telit_tune_repair_needed;
}

void telit_tune_readback_target_rf1(void) {
    s_telit_tune_readback_target = TELIT_TUNE_RF1;
    telit_tune_readback_begin();
}

void telit_tune_readback_target_rf2(void) {
    s_telit_tune_readback_target = TELIT_TUNE_RF2;
    telit_tune_readback_begin();
}

void telit_tune_readback_target_rf3(void) {
    s_telit_tune_readback_target = TELIT_TUNE_RF3;
    telit_tune_readback_begin();
}

void telit_tune_readback_target_rf4(void) {
    s_telit_tune_readback_target = TELIT_TUNE_RF4;
    telit_tune_readback_begin();
}

static bool telit_tune_row_applicable(telit_tune_rf_t rf) {
    return telit_tune_repair_applicable() &&
           telit_tune_effective_mask(rf) != 0u;
}

bool telit_tune_rf1_applicable(void) {
    return telit_tune_row_applicable(TELIT_TUNE_RF1);
}

bool telit_tune_rf2_applicable(void) {
    return telit_tune_row_applicable(TELIT_TUNE_RF2);
}

bool telit_tune_rf3_applicable(void) {
    return telit_tune_row_applicable(TELIT_TUNE_RF3);
}

bool telit_tune_rf4_applicable(void) {
    return telit_tune_row_applicable(TELIT_TUNE_RF4);
}

bool telit_tune_build_row(telit_tune_rf_t rf, char *out,
                                 size_t out_cap) {
    uint64_t mask = telit_tune_effective_mask(rf);
    if (out == NULL || out_cap == 0u || mask == 0u ||
        rf >= TELIT_TUNE_RF_COUNT) {
        return false;
    }
    int length = snprintf(out, out_cap, "AT#STUNEANT=1,%llX,%u,%u",
                          (unsigned long long)mask,
                          (unsigned)TELIT_TUNE_CTRL1[rf],
                          (unsigned)TELIT_TUNE_CTRL2[rf]);
    return length > 0 && (size_t)length < out_cap;
}

bool telit_tune_build_rf1(char *out, size_t out_cap) {
    return telit_tune_build_row(TELIT_TUNE_RF1, out, out_cap);
}

bool telit_tune_build_rf2(char *out, size_t out_cap) {
    return telit_tune_build_row(TELIT_TUNE_RF2, out, out_cap);
}

bool telit_tune_build_rf3(char *out, size_t out_cap) {
    return telit_tune_build_row(TELIT_TUNE_RF3, out, out_cap);
}

bool telit_tune_build_rf4(char *out, size_t out_cap) {
    return telit_tune_build_row(TELIT_TUNE_RF4, out, out_cap);
}

static modem_provision_line_t telit_provision_gpio_direction(
    const char *line, uint8_t expected_direction) {
    if (!telit_starts_with(line, "#GPIO:")) {
        return MODEM_PROVISION_LINE_IGNORE;
    }
    telit_csv_view_t fields[2];
    size_t count = 0u;
    uint32_t direction = 0u;
    uint32_t state = 0u;
    if (!telit_view_split_prefixed(line, "#GPIO:", fields, 2u, &count) ||
        count != 2u ||
        !telit_view_parse_u32(fields[0], 20u, &direction) ||
        !telit_view_parse_u32(fields[1], 1u, &state)) {
        return MODEM_PROVISION_LINE_INVALID;
    }
    (void)state; /* Undefined by the manual while an ALT function owns the pin. */
    return direction == expected_direction
               ? MODEM_PROVISION_LINE_MATCH
               : MODEM_PROVISION_LINE_MISMATCH;
}

modem_provision_line_t telit_provision_gpio2_alt16(
    const char *line) {
    return telit_provision_gpio_direction(line, 17u);
}

modem_provision_line_t telit_provision_gpio3_alt17(
    const char *line) {
    return telit_provision_gpio_direction(line, 18u);
}

uint64_t telit_tune_supported_mask(void) {
    return s_telit_tune_supported_mask;
}

bool telit_tune_repair_required(void) {
    return s_telit_tune_repair_needed;
}
