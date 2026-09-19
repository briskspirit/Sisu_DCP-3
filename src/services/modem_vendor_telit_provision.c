/* Telit LE910C1-WWX initialization and provisioning protocol.
 *
 * This translation unit deliberately contains no board or Pico HAL access.
 * Electrical timing and status thresholds below come from the first Rev B2
 * board characterization; all command strings and response grammar stay here.
 *
 * Primary source: Telit LE910Cx ThreadX AT Commands Reference Guide, Rev. 4,
 * 2026-01-29. Persistent and reboot-requiring settings use the generic
 * query/compare/write/verify provisioning engine. */

#include "modem_vendor_telit_internal.h"

#include <string.h>

#include "services/sms_types.h"

static modem_provision_line_t telit_provision_values(
    const char *line, const char *prefix, const uint8_t *expected,
    size_t expected_count);

static modem_provision_line_t telit_provision_cfun5(const char *line) {
    static const uint8_t expected[] = {5u};
    return telit_provision_values(line, "+CFUN:", expected,
                                  sizeof(expected));
}

/* ------------------------------------------------------------------------ */
/* Runtime-safe and protocol setup descriptors.                             */
/* ------------------------------------------------------------------------ */

#define TELIT_INIT_FIELDS(cmd_, timeout_, retries_, recoverable_, degrade_, prereq_, persistence_, parser_) \
        .cmd = (cmd_), \
        .timeout_ms = (timeout_), \
        .retry_limit = (retries_), \
        .recoverable = (recoverable_), \
        .degrade = (degrade_), \
        .prerequisites = (prereq_), \
        .persistence = (persistence_), \
        .parse = (parser_)
#define TELIT_INIT(cmd_, timeout_, retries_, recoverable_, degrade_, prereq_, persistence_, parser_) \
    { TELIT_INIT_FIELDS(cmd_, timeout_, retries_, recoverable_, degrade_, prereq_, persistence_, parser_), \
      .bypass_cts = false }
#define TELIT_INIT_CAPTURE(cmd_, timeout_, retries_, recoverable_, degrade_, prereq_, persistence_, parser_, capture_) \
    { TELIT_INIT_FIELDS(cmd_, timeout_, retries_, recoverable_, degrade_, prereq_, persistence_, parser_), \
      .capture = (capture_), .bypass_cts = false }
#define TELIT_INIT_BOOTSTRAP(cmd_, timeout_, retries_, recoverable_, degrade_, prereq_, persistence_, parser_) \
    { TELIT_INIT_FIELDS(cmd_, timeout_, retries_, recoverable_, degrade_, prereq_, persistence_, parser_), \
      .bypass_cts = true }

static bool telit_init_parse_qss(const char *line) {
    if (!telit_starts_with(line, "#QSS:")) {
        return false;
    }
    telit_qss_state_t state;
    return telit_parse_qss_query(line, &state);
}

static bool telit_init_parse_cfun1(const char *line) {
    telit_csv_view_t fields[1];
    size_t count = 0u;
    uint32_t value = 0u;
    return telit_view_split_prefixed(line, "+CFUN:", fields, 1u, &count) &&
           count == 1u && telit_view_parse_u32(fields[0], 6u, &value) &&
           value == 1u;
}

/* The ordinary pass remains RF-safe in CFUN=4 through antenna provisioning.
 * Once its final CFUN=5 activates the SIM, a focused completion pass enters
 * CFUN=1, applies SIM-owned URCs and the generic sleep-URC RI profile, then
 * crosses one final verified CFUN=5 boundary. */
const modem_init_step_t TELIT_INIT_STEPS[] = {
    /* #CFLO defaults to disabled, so a factory profile need not provide useful
     * command-mode CTS yet. Keep RTS active, bypass CTS only through the exact
     * commands that establish hardware flow control, then use normal CTS-gated
     * writes for every subsequent command. */
    TELIT_INIT_BOOTSTRAP("ATE0",       2500u, 3u, false, MODEM_DEGRADE_NONE,
                         MODEM_INIT_PREREQ_NONE, MODEM_SETTING_PROFILE, NULL),
    TELIT_INIT_BOOTSTRAP("AT+CMEE=2",  2500u, 3u, false, MODEM_DEGRADE_NONE,
                         MODEM_INIT_PREREQ_NONE, MODEM_SETTING_PROFILE, NULL),
    TELIT_INIT_BOOTSTRAP("AT+IFC=2,2", 2500u, 3u, false, MODEM_DEGRADE_NONE,
                         MODEM_INIT_PREREQ_NONE, MODEM_SETTING_PROFILE, NULL),
    TELIT_INIT_BOOTSTRAP("AT#CFLO=1",  2500u, 3u, false, MODEM_DEGRADE_NONE,
                         MODEM_INIT_PREREQ_NONE, MODEM_SETTING_PROFILE, NULL),
    /* Keep RF off until GPIO ALT16/ALT17 and the complete #STUNEANT table
     * have passed strict provisioning readback. CFUN=5 is the final
     * provisioning row, not an init write. */
    TELIT_INIT("AT+CFUN=4",             5000u, 2u, false, MODEM_DEGRADE_NONE,
               MODEM_INIT_PREREQ_NONE, MODEM_SETTING_RUNTIME, NULL),
    TELIT_INIT("AT+CFUN=1",             5000u, 2u, false, MODEM_DEGRADE_NONE,
               MODEM_INIT_PREREQ_SIM_COMPLETION,
               MODEM_SETTING_RUNTIME, NULL),
    TELIT_INIT("AT+CFUN?",              2500u, 2u, false, MODEM_DEGRADE_NONE,
               MODEM_INIT_PREREQ_SIM_COMPLETION,
               MODEM_SETTING_RUNTIME, telit_init_parse_cfun1),
    TELIT_INIT("AT#STUNEANT=?",         5000u, 2u, false, MODEM_DEGRADE_NONE,
               MODEM_INIT_PREREQ_NONE, MODEM_SETTING_RUNTIME,
               telit_init_parse_stune_capability),
    TELIT_INIT_CAPTURE("AT+CGSN",      2500u, 3u, true, MODEM_DEGRADE_NONE,
               MODEM_INIT_PREREQ_NONE, MODEM_SETTING_RUNTIME, NULL,
               MODEM_INIT_CAPTURE_BOARD_IMEI),
    TELIT_INIT("AT#QSS=2",             2500u, 2u, true, MODEM_DEGRADE_NONE,
               MODEM_INIT_PREREQ_NONE, MODEM_SETTING_PROFILE, NULL),
    TELIT_INIT("AT#QSS?",              2500u, 2u, true, MODEM_DEGRADE_NONE,
               MODEM_INIT_PREREQ_NONE, MODEM_SETTING_RUNTIME,
               telit_init_parse_qss),
    TELIT_INIT("AT+CPIN?",             2500u, 10u, false, MODEM_DEGRADE_SIM_GATE,
               MODEM_INIT_PREREQ_NONE, MODEM_SETTING_RUNTIME, NULL),
    TELIT_INIT("AT+CMGF=1",            2500u, 3u, false, MODEM_DEGRADE_NONE,
               MODEM_INIT_PREREQ_SIM_READY, MODEM_SETTING_PROFILE, NULL),
    /* Text-mode +CMT/+CMGR/+CMGL carry <tooa>,<fo>,<pid>,<dcs>,<length>
     * (3GPP) or <tooa>,<tele_id>,<priority>,<enc>,<length> (Telit 3GPP2)
     * only with +CSDH=1. Direct delivery needs them to rebuild a PDU. */
    TELIT_INIT("AT+CSDH=1",            2500u, 3u, false, MODEM_DEGRADE_NONE,
               MODEM_INIT_PREREQ_SIM_READY, MODEM_SETTING_PROFILE, NULL),
    TELIT_INIT("AT+CSCS=\"GSM\"",      2500u, 3u, false, MODEM_DEGRADE_NONE,
               MODEM_INIT_PREREQ_SIM_READY, MODEM_SETTING_PROFILE, NULL),
    TELIT_INIT("AT#CSCSEXT=0",          2500u, 3u, false, MODEM_DEGRADE_NONE,
               MODEM_INIT_PREREQ_SIM_READY, MODEM_SETTING_PROFILE, NULL),
    TELIT_INIT("AT+CSMP=17,167,0,0",   15000u, 2u, true,
               MODEM_DEGRADE_SMS_SETUP, MODEM_INIT_PREREQ_SIM_READY,
               MODEM_SETTING_RUNTIME, NULL),
    /* Direct delivery (<mt>=2): Verizon 3GPP2 messages cannot be read back
     * from the CDMA store on this image ($QCMTI rows fail in every mode), so
     * every carrier's SMS-DELIVER is routed to the host as +CMT, normalized
     * to a 23.040 PDU, and queued for local littlefs storage. Mode 2 buffers the URC while the
     * TA-TE link is reserved and flushes it afterwards; DTR sleep unchanged. */
    TELIT_INIT("AT+CNMI=2,2,0,0,0",    2500u, 3u, false, MODEM_DEGRADE_NONE,
               MODEM_INIT_PREREQ_SIM_READY, MODEM_SETTING_PROFILE, NULL),
    TELIT_INIT("AT+CLIP=1",             2500u, 3u, false, MODEM_DEGRADE_NONE,
               MODEM_INIT_PREREQ_SIM_READY, MODEM_SETTING_PROFILE, NULL),
    TELIT_INIT("AT+CCWA=1",             2500u, 3u, false, MODEM_DEGRADE_NONE,
               MODEM_INIT_PREREQ_SIM_READY, MODEM_SETTING_PROFILE, NULL),
    TELIT_INIT("AT+CEREG=1",            2500u, 3u, false, MODEM_DEGRADE_NONE,
               MODEM_INIT_PREREQ_NONE, MODEM_SETTING_PROFILE, NULL),
    /* Call, SMS, SIM and MWI each have dedicated URCs. Generic +CIEV
     * indicators only create redundant RI wake pulses while the UART sleeps. */
    TELIT_INIT("AT+CMER=2,0,0,0,0",     2500u, 2u, true, MODEM_DEGRADE_NONE,
               MODEM_INIT_PREREQ_NONE, MODEM_SETTING_PROFILE, NULL),
    TELIT_INIT("AT#ECAM=1",             2500u, 3u, false, MODEM_DEGRADE_NONE,
               MODEM_INIT_PREREQ_SIM_READY, MODEM_SETTING_PROFILE, NULL),
    TELIT_INIT("AT#MWI=1",              2500u, 2u, true, MODEM_DEGRADE_NONE,
               MODEM_INIT_PREREQ_SIM_READY, MODEM_SETTING_PROFILE, NULL),
};

_Static_assert(sizeof(TELIT_INIT_STEPS) / sizeof(TELIT_INIT_STEPS[0]) ==
                   TELIT_INIT_STEP_COUNT,
               "Telit init-step count drifted");

#undef TELIT_INIT
#undef TELIT_INIT_CAPTURE
#undef TELIT_INIT_BOOTSTRAP
#undef TELIT_INIT_FIELDS

static modem_provision_line_t telit_provision_values(
    const char *line, const char *prefix, const uint8_t *expected,
    size_t expected_count) {
    if (!telit_starts_with(line, prefix)) {
        return MODEM_PROVISION_LINE_IGNORE;
    }
    if (expected == NULL || expected_count == 0u || expected_count > 5u) {
        return MODEM_PROVISION_LINE_INVALID;
    }
    telit_csv_view_t fields[5];
    size_t count = 0u;
    if (!telit_view_split_prefixed(line, prefix, fields, expected_count,
                                   &count) ||
        count != expected_count) {
        return MODEM_PROVISION_LINE_INVALID;
    }
    bool match = true;
    for (size_t i = 0u; i < count; i++) {
        uint32_t value = 0u;
        if (!telit_view_parse_u32(fields[i], UINT8_MAX, &value)) {
            return MODEM_PROVISION_LINE_INVALID;
        }
        if (value != expected[i]) {
            match = false;
        }
    }
    return match ? MODEM_PROVISION_LINE_MATCH
                 : MODEM_PROVISION_LINE_MISMATCH;
}

static modem_provision_line_t telit_provision_rxdiv(const char *line) {
    static const uint8_t expected[] = {0u, 1u};
    return telit_provision_values(line, "#RXDIV:", expected,
                                  sizeof(expected));
}

static modem_provision_line_t telit_provision_cssn(const char *line) {
    static const uint8_t expected[] = {1u, 1u};
    return telit_provision_values(line, "+CSSN:", expected,
                                  sizeof(expected));
}

static modem_provision_line_t telit_provision_cff(const char *line) {
    if (!telit_starts_with(line, "#CFF:")) {
        return MODEM_PROVISION_LINE_IGNORE;
    }
    telit_csv_view_t fields[3];
    size_t count = 0u;
    uint32_t enable = 0u;
    uint32_t status = 0u;
    if (!telit_view_split_prefixed(line, "#CFF:", fields, 3u, &count) ||
        count < 1u || !telit_view_parse_u32(fields[0], 1u, &enable) ||
        (count > 1u &&
         (count != 3u ||
          !telit_view_parse_u32(fields[1], 1u, &status)))) {
        return MODEM_PROVISION_LINE_INVALID;
    }
    return enable == 1u ? MODEM_PROVISION_LINE_MATCH
                        : MODEM_PROVISION_LINE_MISMATCH;
}

static modem_provision_line_t telit_provision_sled(const char *line) {
    telit_csv_view_t fields[3];
    size_t count = 0u;
    uint32_t mode = 0u;
    uint32_t on_duration = 0u;
    uint32_t off_duration = 0u;

    if (!telit_starts_with(line, "#SLED:")) {
        return MODEM_PROVISION_LINE_IGNORE;
    }
    if (!telit_view_split_prefixed(line, "#SLED:", fields, 3u, &count) ||
        count != 3u ||
        !telit_view_parse_u32(fields[0], 5u, &mode) ||
        !telit_view_parse_u32(fields[1], 100u, &on_duration) ||
        !telit_view_parse_u32(fields[2], 100u, &off_duration) ||
        on_duration == 0u || off_duration == 0u) {
        return MODEM_PROVISION_LINE_INVALID;
    }
    return mode == 5u ? MODEM_PROVISION_LINE_MATCH
                      : MODEM_PROVISION_LINE_MISMATCH;
}

static modem_provision_line_t telit_provision_gps_startup(const char *line) {
    static const uint8_t expected[] = {0u};
    return telit_provision_values(line, "$GPSPSAV:", expected,
                                  sizeof(expected));
}

static modem_provision_line_t telit_provision_gps_runtime(const char *line) {
    static const uint8_t expected[] = {0u};
    return telit_provision_values(line, "$GPSP:", expected,
                                  sizeof(expected));
}

static modem_provision_line_t telit_provision_scan_timer(const char *line) {
    uint16_t seconds = 0u;
    modem_diag_line_result_t result =
        telit_parse_scan_timer_value(line, &seconds);
    if (result == MODEM_DIAG_LINE_IGNORE) {
        return MODEM_PROVISION_LINE_IGNORE;
    }
    if (result != MODEM_DIAG_LINE_ACCEPT) {
        return MODEM_PROVISION_LINE_INVALID;
    }
    return seconds == 60u ? MODEM_PROVISION_LINE_MATCH
                          : MODEM_PROVISION_LINE_MISMATCH;
}

static modem_provision_line_t telit_provision_auto_profile(const char *line) {
    if (!telit_starts_with(line, "#FWAUTOSIM:")) {
        return MODEM_PROVISION_LINE_IGNORE;
    }
    uint8_t mode = 0u;
    if (!telit_parse_fwautosim(line, &mode)) {
        return MODEM_PROVISION_LINE_INVALID;
    }
    return mode == 1u ? MODEM_PROVISION_LINE_MATCH
                      : MODEM_PROVISION_LINE_MISMATCH;
}

static modem_provision_line_t telit_provision_ecamurc(const char *line) {
    static const uint8_t expected[] = {1u};
    return telit_provision_values(line, "#ECAMURC:", expected,
                                  sizeof(expected));
}

static modem_provision_line_t telit_provision_dviext(const char *line) {
    static const uint8_t expected[] = {1u, 1u, 0u, 0u, 0u};
    return telit_provision_values(line, "#DVIEXT:", expected,
                                  sizeof(expected));
}

static modem_provision_line_t telit_provision_dvi(const char *line) {
    static const uint8_t expected[] = {1u, 2u, 1u};
    return telit_provision_values(line, "#DVI:", expected,
                                  sizeof(expected));
}

static modem_provision_line_t telit_provision_ri_duration(
    const char *line, const char *prefix, uint32_t expected_ms) {
    if (!telit_starts_with(line, prefix)) {
        return MODEM_PROVISION_LINE_IGNORE;
    }
    telit_csv_view_t fields[1];
    size_t count = 0u;
    uint32_t duration_ms = 0u;
    if (!telit_view_split_prefixed(line, prefix, fields, 1u, &count) ||
        count != 1u ||
        !telit_view_parse_u32(fields[0], 1150u, &duration_ms)) {
        return MODEM_PROVISION_LINE_INVALID;
    }
    return duration_ms == expected_ms ? MODEM_PROVISION_LINE_MATCH
                                      : MODEM_PROVISION_LINE_MISMATCH;
}

static modem_provision_line_t telit_provision_wkio(const char *line) {
    static const uint8_t expected[] = {0u, 0u, 2u, 1u};
    static const uint8_t maximum[] = {1u, 2u, 3u, 60u};
    telit_csv_view_t fields[4];
    size_t count = 0u;
    bool match = true;

    if (!telit_starts_with(line, "#WKIO:")) {
        return MODEM_PROVISION_LINE_IGNORE;
    }
    if (!telit_view_split_prefixed(line, "#WKIO:", fields, 4u, &count) ||
        count != 4u) {
        return MODEM_PROVISION_LINE_INVALID;
    }
    for (size_t i = 0u; i < count; i++) {
        uint32_t value = 0u;
        if (!telit_view_parse_u32(fields[i], maximum[i], &value) ||
            (i == 3u && value == 0u)) {
            return MODEM_PROVISION_LINE_INVALID;
        }
        if (value != expected[i]) {
            match = false;
        }
    }
    return match ? MODEM_PROVISION_LINE_MATCH
                 : MODEM_PROVISION_LINE_MISMATCH;
}

static modem_provision_line_t telit_provision_ring_profile(const char *line) {
    static const char prefix[] = "RI (C125) OPTIONS";
    const char *setting;

    const char *profile = line == NULL ? NULL : strstr(line, prefix);
    if (profile == NULL) {
        return MODEM_PROVISION_LINE_IGNORE;
    }
    setting = strstr(profile, "\\R");
    if (setting == NULL || setting[2] < '0' || setting[2] > '2' ||
        setting[3] != '=') {
        return MODEM_PROVISION_LINE_INVALID;
    }
    return setting[2] == '2' ? MODEM_PROVISION_LINE_MATCH
                             : MODEM_PROVISION_LINE_MISMATCH;
}

static modem_provision_line_t telit_provision_e2smsri(const char *line) {
    return telit_provision_ri_duration(line, "#E2SMSRI:", 0u);
}

static modem_provision_line_t telit_provision_psmri(
    const char *line) {
    return telit_provision_ri_duration(line, "#PSMRI:", 1000u);
}

static const char TELIT_SMS_WAKE_SAVED_PROFILE_SET[] =
    "AT#WKIO=0;#E2SMSRI=0;\\R2&W0";

#define TELIT_PROVISION(query_, set_, timeout_, retries_, recoverable_, \
                        degrade_, prereq_, persistence_, parser_) \
    { \
        .query_cmd = (query_), \
        .set_cmd = (set_), \
        .timeout_ms = (timeout_), \
        .retry_limit = (retries_), \
        .recoverable = (recoverable_), \
        .degrade = (degrade_), \
        .prerequisites = (prereq_), \
        .persistence = (persistence_), \
        .parse_readback = (parser_), \
    }
#define TELIT_PROVISION_IF(query_, set_, timeout_, retries_, recoverable_, \
                           degrade_, prereq_, persistence_, parser_, \
                           applicable_) \
    { \
        .query_cmd = (query_), \
        .set_cmd = (set_), \
        .timeout_ms = (timeout_), \
        .retry_limit = (retries_), \
        .recoverable = (recoverable_), \
        .degrade = (degrade_), \
        .prerequisites = (prereq_), \
        .persistence = (persistence_), \
        .parse_readback = (parser_), \
        .applicable = (applicable_), \
    }

/* Order matters: carrier policy before hardware, then SIM storage and ECAM.
 * All mismatches are written once and read back; NVM_REBOOT rows share one
 * controlled reboot after the full pass. DVI remains a separately gated step:
 * ordinary boot must not change an unqualified voice transport. */
const modem_provision_step_t TELIT_PROVISION_STEPS[] = {
    /* Enable persistent SIM-based selection, not a fixed carrier or one-shot
     * mode. Query/repair before board settings; never issue FWSWITCH here. */
    TELIT_PROVISION("AT#FWAUTOSIM?", "AT#FWAUTOSIM=1", 5000u, 2u, true,
                    MODEM_DEGRADE_NONE, MODEM_INIT_PREREQ_NONE,
                    MODEM_SETTING_NVM, telit_provision_auto_profile),
    {
        .query_cmd = "AT#STUNEANT?",
        .timeout_ms = 5000u,
        .retry_limit = 2u,
        .recoverable = false,
        .degrade = MODEM_DEGRADE_NONE,
        .prerequisites = MODEM_INIT_PREREQ_NONE,
        .persistence = MODEM_SETTING_NVM,
        .parse_readback = telit_provision_stune_discover,
        .query_only = true,
    },
    {
        .query_cmd = "AT#GTUNEANT?",
        .timeout_ms = 5000u,
        .retry_limit = 2u,
        .recoverable = false,
        .degrade = MODEM_DEGRADE_NONE,
        .prerequisites = MODEM_INIT_PREREQ_NONE,
        .persistence = MODEM_SETTING_NVM,
        .parse_readback = telit_provision_gtune_line,
        .query_only = true,
        .applicable = telit_tune_table_discovery_applicable,
        .readback_begin = telit_tune_readback_begin,
        .readback_finish = telit_tune_discovery_finish,
    },
    TELIT_PROVISION_IF("AT#STUNEANT?", "AT#STUNEANT=0", 5000u, 2u,
                       false, MODEM_DEGRADE_NONE, MODEM_INIT_PREREQ_NONE,
                       MODEM_SETTING_NVM, telit_provision_stune_disabled,
                       telit_tune_repair_applicable),
    TELIT_PROVISION_IF("AT#GPIO=2,2", "AT#GPIO=2,0,17", 5000u, 2u,
                       false, MODEM_DEGRADE_NONE, MODEM_INIT_PREREQ_NONE,
                       MODEM_SETTING_NVM, telit_provision_gpio2_alt16,
                       telit_tune_repair_applicable),
    TELIT_PROVISION_IF("AT#GPIO=3,2", "AT#GPIO=3,0,18", 5000u, 2u,
                       false, MODEM_DEGRADE_NONE, MODEM_INIT_PREREQ_NONE,
                       MODEM_SETTING_NVM, telit_provision_gpio3_alt17,
                       telit_tune_repair_applicable),
    /* Program non-default states first; RF1/00 last consumes the complement,
     * including the deliberate WWX B7 fallback. Every row is capability-
     * intersected, so the same firmware also accepts NA/NF masks. */
    {
        .query_cmd = "AT#GTUNEANT?",
        .timeout_ms = 5000u,
        .retry_limit = 2u,
        .recoverable = false,
        .degrade = MODEM_DEGRADE_NONE,
        .prerequisites = MODEM_INIT_PREREQ_NONE,
        .persistence = MODEM_SETTING_NVM,
        .parse_readback = telit_provision_gtune_line,
        .applicable = telit_tune_rf2_applicable,
        .build_set_cmd = telit_tune_build_rf2,
        .readback_begin = telit_tune_readback_target_rf2,
        .readback_finish = telit_tune_row_finish,
    },
    {
        .query_cmd = "AT#GTUNEANT?",
        .timeout_ms = 5000u,
        .retry_limit = 2u,
        .recoverable = false,
        .degrade = MODEM_DEGRADE_NONE,
        .prerequisites = MODEM_INIT_PREREQ_NONE,
        .persistence = MODEM_SETTING_NVM,
        .parse_readback = telit_provision_gtune_line,
        .applicable = telit_tune_rf3_applicable,
        .build_set_cmd = telit_tune_build_rf3,
        .readback_begin = telit_tune_readback_target_rf3,
        .readback_finish = telit_tune_row_finish,
    },
    {
        .query_cmd = "AT#GTUNEANT?",
        .timeout_ms = 5000u,
        .retry_limit = 2u,
        .recoverable = false,
        .degrade = MODEM_DEGRADE_NONE,
        .prerequisites = MODEM_INIT_PREREQ_NONE,
        .persistence = MODEM_SETTING_NVM,
        .parse_readback = telit_provision_gtune_line,
        .applicable = telit_tune_rf4_applicable,
        .build_set_cmd = telit_tune_build_rf4,
        .readback_begin = telit_tune_readback_target_rf4,
        .readback_finish = telit_tune_row_finish,
    },
    {
        .query_cmd = "AT#GTUNEANT?",
        .timeout_ms = 5000u,
        .retry_limit = 2u,
        .recoverable = false,
        .degrade = MODEM_DEGRADE_NONE,
        .prerequisites = MODEM_INIT_PREREQ_NONE,
        .persistence = MODEM_SETTING_NVM,
        .parse_readback = telit_provision_gtune_line,
        .applicable = telit_tune_rf1_applicable,
        .build_set_cmd = telit_tune_build_rf1,
        .readback_begin = telit_tune_readback_target_rf1,
        .readback_finish = telit_tune_row_finish,
    },
    {
        .query_cmd = "AT#GTUNEANT?",
        .timeout_ms = 5000u,
        .retry_limit = 2u,
        .recoverable = false,
        .degrade = MODEM_DEGRADE_NONE,
        .prerequisites = MODEM_INIT_PREREQ_NONE,
        .persistence = MODEM_SETTING_NVM,
        .parse_readback = telit_provision_gtune_line,
        .query_only = true,
        .applicable = telit_tune_repair_applicable,
        .readback_begin = telit_tune_readback_begin,
        .readback_finish = telit_tune_final_finish,
    },
    TELIT_PROVISION("AT#RXDIV?", "AT#RXDIV=0,1", 10000u, 2u, false,
                    MODEM_DEGRADE_NONE, MODEM_INIT_PREREQ_NONE,
                    MODEM_SETTING_NVM_REBOOT, telit_provision_rxdiv),
    /* Hardware Guide r46 Table 20 quotes idle current only with STAT_LED off.
     * #SLED is persisted solely by #SLEDSAV, so repair and save it atomically;
     * strict readback prevents an unsupported/malformed response from being
     * mistaken for the documented mode 5. */
    TELIT_PROVISION("AT#SLED?", "AT#SLED=5;#SLEDSAV", 5000u, 2u, false,
                    MODEM_DEGRADE_NONE, MODEM_INIT_PREREQ_NONE,
                    MODEM_SETTING_NVM, telit_provision_sled),
    TELIT_PROVISION("AT$GPSPSAV?", "AT$GPSPSAV=0", 5000u, 2u, false,
                    MODEM_DEGRADE_NONE, MODEM_INIT_PREREQ_NONE,
                    MODEM_SETTING_NVM, telit_provision_gps_startup),
    TELIT_PROVISION("AT$GPSP?", "AT$GPSP=0", 5000u, 2u, false,
                    MODEM_DEGRADE_NONE, MODEM_INIT_PREREQ_NONE,
                    MODEM_SETTING_RUNTIME, telit_provision_gps_runtime),
    /* Carrier-profile restores can return the no-coverage scan pause to 5 s.
     * NWSCANTMR auto-saves and needs no reboot; verify before resuming RF. */
    TELIT_PROVISION("AT#NWSCANTMR?", "AT#NWSCANTMR=60", 5000u, 2u, true,
                    MODEM_DEGRADE_NONE, MODEM_INIT_PREREQ_NONE,
                    MODEM_SETTING_NVM, telit_provision_scan_timer),
    TELIT_PROVISION("AT#ECAMURC?", "AT#ECAMURC=1", 5000u, 2u, false,
                    MODEM_DEGRADE_NONE, MODEM_INIT_PREREQ_SIM_READY,
                    MODEM_SETTING_NVM_REBOOT, telit_provision_ecamurc),
    /* Board-1 OAP bench qualification proved this exact 16 kHz I2S,
     * module-master path end to end. DVIEXT is system-persistent and may need
     * one reboot after its first change; #DVI itself is treated as runtime
     * state because ThreadX persistence is not guaranteed. Both are queried
     * and repaired before READY, never written blindly or during a call. */
    TELIT_PROVISION("AT#DVIEXT?", "AT#DVIEXT=1,1", 5000u, 2u, true,
                    MODEM_DEGRADE_AUDIO, MODEM_INIT_PREREQ_NONE,
                    MODEM_SETTING_NVM_REBOOT, telit_provision_dviext),
    TELIT_PROVISION("AT#DVI?", "AT#DVI=1,2,1", 5000u, 2u, true,
                    MODEM_DEGRADE_AUDIO, MODEM_INIT_PREREQ_NONE,
                    MODEM_SETTING_RUNTIME, telit_provision_dvi),
    /* Nokia's standby divert state and redirected-call UI need typed network
     * notifications. Both settings are profile-owned and SIM-dependent, so
     * repair them in the focused completion pass instead of blind-writing at
     * every cold init. */
    TELIT_PROVISION("AT+CSSN?", "AT+CSSN=1,1", 5000u, 2u, true,
                    MODEM_DEGRADE_NONE,
                    MODEM_INIT_PREREQ_SIM_READY |
                        MODEM_INIT_PREREQ_SIM_COMPLETION,
                    MODEM_SETTING_PROFILE, telit_provision_cssn),
    TELIT_PROVISION("AT#CFF?", "AT#CFF=1", 5000u, 2u, true,
                    MODEM_DEGRADE_NONE,
                    MODEM_INIT_PREREQ_SIM_READY |
                        MODEM_INIT_PREREQ_SIM_COMPLETION,
                    MODEM_SETTING_PROFILE, telit_provision_cff),
    /* LE910Cx Software User Guide r17 section 5.2.10 requires an SMS
     * indication via CNMI (its example is 1,1; the init table now applies
     * 2,2 for direct delivery, bench-verified to arrive after DTR sleep, see
     * docs/sms_direct_delivery_design.md) and nonzero PSMRI before entering
     * CFUN=5. The AT guide says PSMRI is ignored
     * while an event-specific RING source is enabled, so WKIO and E2SMSRI must
     * both be disabled. Preserve \R2 for ordinary incoming-call RI behavior.
     *
     * All four readbacks are completion-only: they execute after CFUN=1 and
     * before the qualifying CFUN=5 row. The first three repair and save only
     * genuine profile state on mismatch. PSMRI is then applied and verified on
     * every pass: the WWX clears its live value across this CFUN lifecycle, and
     * saving the same profile on every phone start would cause needless wear. */
    TELIT_PROVISION("AT#WKIO?", TELIT_SMS_WAKE_SAVED_PROFILE_SET,
                    5000u, 2u, false,
                    MODEM_DEGRADE_NONE,
                    MODEM_INIT_PREREQ_SIM_READY |
                        MODEM_INIT_PREREQ_SIM_COMPLETION,
                    MODEM_SETTING_NVM, telit_provision_wkio),
    TELIT_PROVISION("AT&V", TELIT_SMS_WAKE_SAVED_PROFILE_SET,
                    5000u, 2u, false,
                    MODEM_DEGRADE_NONE,
                    MODEM_INIT_PREREQ_SIM_READY |
                        MODEM_INIT_PREREQ_SIM_COMPLETION,
                    MODEM_SETTING_NVM,
                    telit_provision_ring_profile),
    TELIT_PROVISION("AT#E2SMSRI?", TELIT_SMS_WAKE_SAVED_PROFILE_SET,
                    5000u, 2u, false, MODEM_DEGRADE_NONE,
                    MODEM_INIT_PREREQ_SIM_READY |
                        MODEM_INIT_PREREQ_SIM_COMPLETION,
                    MODEM_SETTING_NVM, telit_provision_e2smsri),
    {
        .query_cmd = "AT#PSMRI?",
        .set_cmd = "AT#PSMRI=1000",
        .timeout_ms = 5000u,
        .retry_limit = 2u,
        .recoverable = false,
        .degrade = MODEM_DEGRADE_NONE,
        .prerequisites = MODEM_INIT_PREREQ_SIM_READY |
                         MODEM_INIT_PREREQ_SIM_COMPLETION,
        .persistence = MODEM_SETTING_PROFILE,
        .parse_readback = telit_provision_psmri,
        .set_each_pass = true,
    },
    /* RF may only resume after the antenna table and all persistent safety
     * rows have verified. This is runtime state and causes no NVM wear. */
    TELIT_PROVISION("AT+CFUN?", "AT+CFUN=5", 5000u, 2u, false,
                    MODEM_DEGRADE_NONE, MODEM_INIT_PREREQ_NONE,
                    MODEM_SETTING_RUNTIME, telit_provision_cfun5),
    {
        .query_cmd = "AT+CFUN?",
        .set_cmd = "AT+CFUN=5",
        .timeout_ms = 5000u,
        .retry_limit = 2u,
        .recoverable = false,
        .degrade = MODEM_DEGRADE_NONE,
        .prerequisites = MODEM_INIT_PREREQ_SIM_READY |
                         MODEM_INIT_PREREQ_SIM_COMPLETION,
        .persistence = MODEM_SETTING_RUNTIME,
        .parse_readback = telit_provision_cfun5,
        .qualifies_sms_wake = true,
    },
};

_Static_assert(sizeof(TELIT_PROVISION_STEPS) /
                       sizeof(TELIT_PROVISION_STEPS[0]) ==
                   TELIT_PROVISION_STEP_COUNT,
               "Telit provision-step count drifted");

#undef TELIT_PROVISION
#undef TELIT_PROVISION_IF
