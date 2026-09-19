/* Host tests for the production Telit LE910C1-WWX backend.
 *
 * The backend and its private protocol modules are linked as independent
 * translation units, matching the production boundary. */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "../src/services/modem_vendor_telit_internal.h"
#include "services/modem_sms_direct.h"
#include "services/sms_picture_codec.h"

/* Payloads are byte ranges, never C strings. */
#define BYTES(s) (const uint8_t *)(s), strlen(s)

static int s_failures;

static void check(bool condition, const char *message) {
    if (!condition) {
        fprintf(stderr, "FAIL: %s\n", message);
        s_failures++;
    }
}

static void check_string(const char *actual, const char *expected,
                         const char *message) {
    if (strcmp(actual, expected) != 0) {
        fprintf(stderr, "FAIL: %s (got '%s', expected '%s')\n",
                message, actual, expected);
        s_failures++;
    }
}

static const modem_init_step_t *find_init_step(const char *command) {
    for (uint8_t i = 0u; i < g_modem_vendor.init_step_count; i++) {
        if (strcmp(g_modem_vendor.init_steps[i].cmd, command) == 0) {
            return &g_modem_vendor.init_steps[i];
        }
    }
    return NULL;
}

static void check_init_policy(const char *command, uint8_t prerequisites,
                              modem_setting_persistence_t persistence) {
    const modem_init_step_t *step = find_init_step(command);
    check(step != NULL, "expected Telit init command is present");
    if (step != NULL) {
        check(step->prerequisites == prerequisites,
              "Telit init SIM prerequisite matches manual");
        check(step->persistence == persistence,
              "Telit init persistence matches manual");
    }
}

static void reset_diag_snapshot(modem_diag_snapshot_t *snapshot) {
    memset(snapshot, 0, sizeof(*snapshot));
}

static const modem_diag_query_t *find_diag_query(const char *command) {
    for (uint8_t i = 0u; i < g_modem_vendor.diag_query_count; i++) {
        if (strcmp(g_modem_vendor.diag_queries[i].cmd, command) == 0) {
            return &g_modem_vendor.diag_queries[i];
        }
    }
    return NULL;
}

static bool build_call(call_txn_kind_t kind, const char *number,
                       uint8_t target, const char *expected,
                       uint32_t expected_timeout) {
    char command[64];
    uint32_t timeout = 0u;
    bool ok = g_modem_vendor.call.build_command(
        kind, number, target, command, sizeof(command), &timeout);
    check(ok, "call builder accepted supported operation");
    if (!ok) {
        return false;
    }
    check_string(command, expected, "call command");
    check(timeout == expected_timeout, "call timeout");
    return true;
}

static const modem_provision_step_t *find_provision_step(
    const char *query_command) {
    unsigned ordinal = 0u;
    for (uint8_t i = 0u; i < g_modem_vendor.provision_step_count; i++) {
        if (strcmp(g_modem_vendor.provision_steps[i].query_cmd,
                   query_command) == 0) {
            if (ordinal == 0u) {
                return &g_modem_vendor.provision_steps[i];
            }
            ordinal--;
        }
    }
    return NULL;
}

static const modem_provision_step_t *find_provision_step_nth(
    const char *query_command, unsigned ordinal) {
    for (uint8_t i = 0u; i < g_modem_vendor.provision_step_count; i++) {
        if (strcmp(g_modem_vendor.provision_steps[i].query_cmd,
                   query_command) == 0) {
            if (ordinal == 0u) {
                return &g_modem_vendor.provision_steps[i];
            }
            ordinal--;
        }
    }
    return NULL;
}

static void test_production_descriptor(void) {
    check(g_modem_vendor.available,
          "bench-gated Telit backend is available");
    check(g_modem_vendor.capabilities ==
              (MODEM_VENDOR_CAP_DTR_SLEEP |
               MODEM_VENDOR_CAP_VOICE_TRANSPORT),
          "bench-qualified DTR sleep and DVI voice are claimed");
    check_string(g_modem_vendor.power.off_cmd, "AT#SHDN",
                 "manual graceful shutdown command");
    check(g_modem_vendor.power.rail_settle_ms == 20u &&
              g_modem_vendor.power.rail_power_good_timeout_ms == 250u &&
              g_modem_vendor.power.pwron_pulse_ms == 1200u &&
              g_modem_vendor.power.ready_budget_ms == 30000u &&
              g_modem_vendor.power.off_ack_timeout_ms == 3000u &&
              g_modem_vendor.power.off_sense_timeout_ms == 25000u &&
              g_modem_vendor.power.graceful_off_pulse_ms == 3000u &&
              g_modem_vendor.power.graceful_off_sense_timeout_ms == 60000u &&
              g_modem_vendor.power.emergency_off_pulse_ms == 250u &&
              g_modem_vendor.power.emergency_off_sense_timeout_ms == 5000u &&
              !g_modem_vendor.power.cut_rail_on_off_timeout,
          "Rev B2 measured electrical timing and bounded safe shutdown policy");
    check(g_modem_vendor.wake.strategy == MODEM_WAKE_DTR_CTS &&
              g_modem_vendor.wake.awake_window_ms == 1000u &&
              g_modem_vendor.wake.ri_release_timeout_ms == 1500u &&
              g_modem_vendor.wake.dtr_wake_timeout_ms == 500u &&
              g_modem_vendor.wake.probe_bypass_cts,
          "bench-measured DTR/CTS policy exported");
    check(g_modem_vendor.sms_wake.arm_cmd == NULL &&
              g_modem_vendor.sms_wake.timeout_ms == 0u &&
              g_modem_vendor.sms_wake.retry_limit == 0u &&
              g_modem_vendor.sms_wake.command_invalidates_arm != NULL &&
              !g_modem_vendor.sms_wake.command_invalidates_arm(
                  "AT#PSMRI?") &&
              !g_modem_vendor.sms_wake.command_invalidates_arm(
                  "AT+CPMS=\"ME\",\"ME\",\"ME\"") &&
              g_modem_vendor.sms_wake.command_invalidates_arm(
                  "AT#PSMRI=1000") &&
              g_modem_vendor.sms_wake.command_invalidates_arm("AT\\R1") &&
              g_modem_vendor.sms_wake.command_invalidates_arm(
                  "AT+CFUN=5") &&
              g_modem_vendor.sms_wake.qualified_by_sim_completion &&
              g_modem_vendor.sms_wake.sim_activation_window_ms == 10000u,
          "pre-CFUN SMS wake lifecycle stays in the Telit vendor contract");
    check(g_modem_vendor.call.capabilities == MODEM_CALL_CAPABILITY_ALL &&
              g_modem_vendor.call.build_command != NULL &&
              g_modem_vendor.call.build_dtmf_command != NULL,
          "manual-supported call operations exposed at protocol layer");
    check(g_modem_vendor.call.progress_finals_may_complete_command,
          "usual Telit finals can complete call commands");
    check(g_modem_vendor.provision_schema_version == 13u,
          "Telit provisioning contract has an explicit schema version");
    check(g_modem_vendor.supplementary.supported &&
              g_modem_vendor.supplementary.call_forward_step_count != NULL &&
              g_modem_vendor.supplementary.build_call_forward_step != NULL &&
              g_modem_vendor.supplementary.parse_call_forward_row != NULL &&
              strcmp(g_modem_vendor.supplementary.call_forward_response_prefix,
                     "+CCFC:") == 0 &&
              strcmp(g_modem_vendor.supplementary.voice_mailbox_query_cmd,
                     "AT#MBN") == 0 &&
              strcmp(g_modem_vendor.supplementary.voice_mailbox_response_prefix,
                     "#MBN:") == 0 &&
              strcmp(g_modem_vendor.supplementary.message_waiting_query_cmd,
                     "AT#MWI?") == 0 &&
              strcmp(
                  g_modem_vendor.supplementary.message_waiting_response_prefix,
                  "#MWI:") == 0 &&
              g_modem_vendor.supplementary.parse_message_waiting_row != NULL,
          "call-forwarding, SIM mailbox, and MWI boundary exported");

    modem_power_observation_t observation = {
        .module_status_valid = true,
        .module_status_raw = 2214u,
        .module_status_mv = 1784u,
    };
    check(g_modem_vendor.power_is_on(&observation),
          "measured high PWRMON classifies module on");
    g_modem_vendor.off_sense_begin(1000u);
    check(!g_modem_vendor.off_complete(1000u, &observation),
          "high PWRMON cannot complete shutdown");
    observation.module_status_raw = 36u;
    observation.module_status_mv = 29u;
    check(!g_modem_vendor.power_is_on(&observation),
          "measured low PWRMON does not classify module on");
    check(!g_modem_vendor.off_complete(1100u, &observation) &&
              !g_modem_vendor.off_complete(1599u, &observation) &&
              g_modem_vendor.off_complete(1600u, &observation),
          "shutdown requires 500 ms continuously low PWRMON");

    bool saw_ecam = false;
    bool saw_ecamurc = false;
    for (uint8_t i = 0u; i < g_modem_vendor.init_step_count; i++) {
        const modem_init_step_t *step = &g_modem_vendor.init_steps[i];
        check(step->cmd != NULL && step->timeout_ms > 0u,
              "each protocol init descriptor is bounded");
        check(strstr(step->cmd, "AT#FWSWITCH=") == NULL &&
                  strstr(step->cmd, "AT#FWAUTOSIM=") == NULL &&
                  strstr(step->cmd, "AT#ISMSCFG=") == NULL &&
                  strstr(step->cmd, "AT#RXDIV=") == NULL &&
                  strstr(step->cmd, "AT#DVI") == NULL,
              "destructive/reboot/audio provisioning absent from init");
        if (strcmp(step->cmd, "AT#ECAM=1") == 0) {
            saw_ecam = true;
            check(step->prerequisites == MODEM_INIT_PREREQ_SIM_READY &&
                      step->persistence == MODEM_SETTING_PROFILE,
                  "ECAM is SIM-gated and classified as profile state");
        } else if (strcmp(step->cmd, "AT#ECAMURC=1") == 0) {
            saw_ecamurc = true;
        }
    }
    check(saw_ecam && !saw_ecamurc,
          "ECAM runtime mode is initialized but persistent ECAMURC is not blind-written");

    check_init_policy("ATE0", MODEM_INIT_PREREQ_NONE,
                      MODEM_SETTING_PROFILE);
    check_init_policy("AT+CGSN", MODEM_INIT_PREREQ_NONE,
                      MODEM_SETTING_RUNTIME);
    check_init_policy("AT+CMEE=2", MODEM_INIT_PREREQ_NONE,
                      MODEM_SETTING_PROFILE);
    check_init_policy("AT+IFC=2,2", MODEM_INIT_PREREQ_NONE,
                      MODEM_SETTING_PROFILE);
    check_init_policy("AT#CFLO=1", MODEM_INIT_PREREQ_NONE,
                      MODEM_SETTING_PROFILE);
    check_init_policy("AT+CFUN=4", MODEM_INIT_PREREQ_NONE,
                      MODEM_SETTING_RUNTIME);
    check_init_policy("AT+CFUN=1", MODEM_INIT_PREREQ_SIM_COMPLETION,
                      MODEM_SETTING_RUNTIME);
    check_init_policy("AT+CFUN?", MODEM_INIT_PREREQ_SIM_COMPLETION,
                      MODEM_SETTING_RUNTIME);
    const modem_init_step_t *cfun1_verify = find_init_step("AT+CFUN?");
    check(cfun1_verify != NULL && cfun1_verify->parse != NULL &&
              cfun1_verify->parse("+CFUN: 1") &&
              !cfun1_verify->parse("+CFUN: 5") &&
              !cfun1_verify->parse("+CFUN: malformed"),
          "focused completion strictly verifies CFUN=1 before SIM setup");
    check_init_policy("AT#STUNEANT=?", MODEM_INIT_PREREQ_NONE,
                      MODEM_SETTING_RUNTIME);
    check_init_policy("AT#QSS=2", MODEM_INIT_PREREQ_NONE,
                      MODEM_SETTING_PROFILE);
    check_init_policy("AT#QSS?", MODEM_INIT_PREREQ_NONE,
                      MODEM_SETTING_RUNTIME);
    check_init_policy("AT+CPIN?", MODEM_INIT_PREREQ_NONE,
                      MODEM_SETTING_RUNTIME);
    check_init_policy("AT+CMGF=1", MODEM_INIT_PREREQ_SIM_READY | MODEM_INIT_PREREQ_SIM_COMPLETION,
                      MODEM_SETTING_PROFILE);
    check_init_policy("AT+CSCS=\"GSM\"", MODEM_INIT_PREREQ_SIM_READY | MODEM_INIT_PREREQ_SIM_COMPLETION,
                      MODEM_SETTING_PROFILE);
    check_init_policy("AT+CSMP=17,167,0,0",
                      MODEM_INIT_PREREQ_SIM_READY,
                      MODEM_SETTING_RUNTIME);
    check_init_policy("AT+CSDH=1", MODEM_INIT_PREREQ_SIM_READY | MODEM_INIT_PREREQ_SIM_COMPLETION,
                      MODEM_SETTING_PROFILE);
    check_init_policy("AT+CNMI=2,2,0,0,0", MODEM_INIT_PREREQ_SIM_READY | MODEM_INIT_PREREQ_SIM_COMPLETION,
                      MODEM_SETTING_PROFILE);
    check_init_policy("AT+CLIP=1", MODEM_INIT_PREREQ_SIM_READY,
                      MODEM_SETTING_PROFILE);
    check_init_policy("AT+CCWA=1", MODEM_INIT_PREREQ_SIM_READY,
                      MODEM_SETTING_PROFILE);
    check_init_policy("AT+CEREG=1", MODEM_INIT_PREREQ_NONE,
                      MODEM_SETTING_PROFILE);
    check(find_init_step("AT+CIND=?") == NULL,
          "unused generic indicator discovery omitted");
    check_init_policy("AT+CMER=2,0,0,0,0", MODEM_INIT_PREREQ_NONE,
                      MODEM_SETTING_PROFILE);
    check_init_policy("AT#MWI=1", MODEM_INIT_PREREQ_SIM_READY,
                      MODEM_SETTING_PROFILE);
    check(find_init_step("AT+CFUN=5") == NULL,
          "RF online transition is held until provisioning verifies");

    static const char *const bootstrap_commands[] = {
        "ATE0", "AT+CMEE=2", "AT+IFC=2,2", "AT#CFLO=1",
    };
    for (size_t i = 0u;
         i < sizeof(bootstrap_commands) / sizeof(bootstrap_commands[0]); i++) {
        const modem_init_step_t *step = find_init_step(bootstrap_commands[i]);
        check(step != NULL && step->bypass_cts,
              "factory-flow bootstrap command explicitly bypasses CTS");
    }
    const modem_init_step_t *identity = find_init_step("AT+CGSN");
    check(identity != NULL && !identity->bypass_cts &&
              identity->recoverable &&
              identity->capture == MODEM_INIT_CAPTURE_BOARD_IMEI,
          "CGSN is a recoverable neutral board-identity capture under CTS");

    check(g_modem_vendor.parse_sim_observation("#QSS: 2,0") ==
              MODEM_SIM_OBSERVATION_ABSENT &&
              g_modem_vendor.parse_sim_observation("#QSS: 2,1") ==
              MODEM_SIM_OBSERVATION_PRESENT &&
              g_modem_vendor.parse_sim_observation("#QSS: 2,2") ==
              MODEM_SIM_OBSERVATION_PRESENT &&
              g_modem_vendor.parse_sim_observation("#QSS: 2,3") ==
              MODEM_SIM_OBSERVATION_READY &&
              g_modem_vendor.parse_sim_observation("#QSS: 0") ==
              MODEM_SIM_OBSERVATION_ABSENT &&
              g_modem_vendor.parse_sim_observation("#QSS: 4,3") ==
              MODEM_SIM_OBSERVATION_NONE,
          "QSS query/URC statuses normalize without leaking Telit values");

    const modem_provision_step_t *rxdiv = find_provision_step("AT#RXDIV?");
    const modem_provision_step_t *sled = find_provision_step("AT#SLED?");
    const modem_provision_step_t *gps_start =
        find_provision_step("AT$GPSPSAV?");
    const modem_provision_step_t *gps_runtime =
        find_provision_step("AT$GPSP?");
    const modem_provision_step_t *scan_timer =
        find_provision_step("AT#NWSCANTMR?");
    const modem_provision_step_t *auto_profile =
        find_provision_step("AT#FWAUTOSIM?");
    const modem_provision_step_t *cpms = find_provision_step("AT+CPMS?");
    const modem_provision_step_t *wkio = find_provision_step("AT#WKIO?");
    const modem_provision_step_t *ring_profile =
        find_provision_step("AT&V");
    const modem_provision_step_t *e2smsri =
        find_provision_step("AT#E2SMSRI?");
    const modem_provision_step_t *psmri =
        find_provision_step("AT#PSMRI?");
    const modem_provision_step_t *ecamurc =
        find_provision_step("AT#ECAMURC?");
    const modem_provision_step_t *dviext =
        find_provision_step("AT#DVIEXT?");
    const modem_provision_step_t *dvi =
        find_provision_step("AT#DVI?");
    const modem_provision_step_t *stune =
        find_provision_step("AT#STUNEANT?");
    const modem_provision_step_t *gpio2 =
        find_provision_step("AT#GPIO=2,2");
    const modem_provision_step_t *gpio3 =
        find_provision_step("AT#GPIO=3,2");
    const modem_provision_step_t *cfun =
        find_provision_step_nth("AT+CFUN?", 0u);
    const modem_provision_step_t *cfun_completion =
        find_provision_step_nth("AT+CFUN?", 1u);
    check(rxdiv != NULL && sled != NULL && gps_start != NULL &&
              gps_runtime != NULL && scan_timer != NULL && auto_profile != NULL &&
              cpms == NULL && wkio != NULL && ring_profile != NULL &&
              e2smsri != NULL && psmri != NULL && ecamurc != NULL &&
              dviext != NULL && dvi != NULL &&
              stune != NULL && gpio2 != NULL && gpio3 != NULL && cfun != NULL &&
              cfun_completion != NULL,
          "all approved safety/SIM/event settings use provisioning descriptors");
    for (size_t i = 0u; i < g_modem_vendor.provision_step_count; i++) {
        const char *set = g_modem_vendor.provision_steps[i].set_cmd;
        check(set == NULL || (strstr(set, "#FWSWITCH=") == NULL &&
                                 strstr(set, "&F") == NULL),
              "provisioning never forces a carrier or issues factory restore");
    }
    if (auto_profile != NULL) {
        check(auto_profile == &g_modem_vendor.provision_steps[1] &&
                  auto_profile->prerequisites == MODEM_INIT_PREREQ_NONE &&
                  auto_profile->persistence == MODEM_SETTING_NVM &&
                  auto_profile->recoverable && !auto_profile->set_each_pass &&
                  strcmp(auto_profile->set_cmd, "AT#FWAUTOSIM=1") == 0 &&
                  auto_profile->parse_readback("#FWAUTOSIM: 1") ==
                      MODEM_PROVISION_LINE_MATCH &&
                  auto_profile->parse_readback("#FWAUTOSIM: 0") ==
                      MODEM_PROVISION_LINE_MISMATCH &&
                  auto_profile->parse_readback("#FWAUTOSIM: 2") ==
                      MODEM_PROVISION_LINE_MISMATCH &&
                  auto_profile->parse_readback("#FWAUTOSIM: 3") ==
                      MODEM_PROVISION_LINE_MISMATCH &&
                  auto_profile->parse_readback("#FWAUTOSIMEXP: 1") ==
                      MODEM_PROVISION_LINE_IGNORE,
              "persistent automatic carrier selection is verified before board setup");
        static const char *const invalid_auto_profiles[] = {
            "#FWAUTOSIM:", "#FWAUTOSIM: bad", "#FWAUTOSIM: -1",
            "#FWAUTOSIM: 4", "#FWAUTOSIM: 1,0", "#FWAUTOSIM: 1junk",
        };
        for (size_t i = 0u;
             i < sizeof(invalid_auto_profiles) / sizeof(invalid_auto_profiles[0]);
             i++) {
            check(auto_profile->parse_readback(invalid_auto_profiles[i]) ==
                      MODEM_PROVISION_LINE_INVALID,
                  "auto-selection provisioning rejects malformed readbacks");
        }
    }
    if (rxdiv != NULL) {
        check(rxdiv->persistence == MODEM_SETTING_NVM_REBOOT &&
                  rxdiv->parse_readback("#RXDIV: 0,1") ==
                      MODEM_PROVISION_LINE_MATCH &&
                  rxdiv->parse_readback("#RXDIV: 1,1") ==
                      MODEM_PROVISION_LINE_MISMATCH &&
                  rxdiv->parse_readback("#RXDIV: x,1") ==
                      MODEM_PROVISION_LINE_INVALID,
              "RXDIV provisioning is strict and reboot-classified");
    }
    if (sled != NULL) {
        check(sled->persistence == MODEM_SETTING_NVM &&
                  strcmp(sled->set_cmd, "AT#SLED=5;#SLEDSAV") == 0 &&
                  sled->parse_readback("#SLED: 5,10,10") ==
                      MODEM_PROVISION_LINE_MATCH &&
                  sled->parse_readback("#SLED: 2,10,10") ==
                      MODEM_PROVISION_LINE_MISMATCH &&
                  sled->parse_readback("#SLED: 5,0,10") ==
                      MODEM_PROVISION_LINE_INVALID &&
                  sled->parse_readback("#SLED: 5,10,101") ==
                      MODEM_PROVISION_LINE_INVALID &&
                  sled->parse_readback("#SLED: 5,10") ==
                      MODEM_PROVISION_LINE_INVALID &&
                  sled->parse_readback("#SLED: 5,10,10,1") ==
                      MODEM_PROVISION_LINE_INVALID,
              "STAT_LED is strictly disabled and saved for documented idle current");
    }
    if (scan_timer != NULL) {
        check(scan_timer->persistence == MODEM_SETTING_NVM &&
                  scan_timer->prerequisites == MODEM_INIT_PREREQ_NONE &&
                  scan_timer->recoverable &&
                  scan_timer->degrade == MODEM_DEGRADE_NONE &&
                  !scan_timer->set_each_pass &&
                  strcmp(scan_timer->set_cmd, "AT#NWSCANTMR=60") == 0 &&
                  scan_timer->parse_readback("#NWSCANTMR: 60") ==
                      MODEM_PROVISION_LINE_MATCH &&
                  scan_timer->parse_readback("#NWSCANTMR: 5") ==
                      MODEM_PROVISION_LINE_MISMATCH &&
                  scan_timer->parse_readback("#NWSCANTMR: 3600") ==
                      MODEM_PROVISION_LINE_MISMATCH &&
                  scan_timer->parse_readback("#NWSCANTMREXP: 60") ==
                      MODEM_PROVISION_LINE_IGNORE,
              "no-coverage scan pause is verified without blind NVM writes");
        static const char *const invalid_scan_timers[] = {
            "#NWSCANTMR:", "#NWSCANTMR: x", "#NWSCANTMR: -60",
            "#NWSCANTMR: 4", "#NWSCANTMR: 3601", "#NWSCANTMR: 60,5",
            "#NWSCANTMR: 60junk",
        };
        for (size_t i = 0u;
             i < sizeof(invalid_scan_timers) / sizeof(invalid_scan_timers[0]);
             i++) {
            check(scan_timer->parse_readback(invalid_scan_timers[i]) ==
                      MODEM_PROVISION_LINE_INVALID,
                  "scan pause rejects malformed and out-of-range readbacks");
        }
    }
    if (ecamurc != NULL) {
        check(ecamurc->prerequisites == MODEM_INIT_PREREQ_SIM_READY &&
                  ecamurc->persistence == MODEM_SETTING_NVM_REBOOT &&
                  ecamurc->parse_readback("#ECAMURC: 1") ==
                      MODEM_PROVISION_LINE_MATCH,
              "ECAMURC provisioning is SIM-gated and reboot-classified");
    }
    if (dviext != NULL) {
        check(dviext->recoverable &&
                  dviext->degrade == MODEM_DEGRADE_AUDIO &&
                  dviext->prerequisites == MODEM_INIT_PREREQ_NONE &&
                  dviext->persistence == MODEM_SETTING_NVM_REBOOT &&
                  strcmp(dviext->set_cmd, "AT#DVIEXT=1,1") == 0 &&
                  dviext->parse_readback("#DVIEXT: 1,1,0,0,0") ==
                      MODEM_PROVISION_LINE_MATCH &&
                  dviext->parse_readback("#DVIEXT: 0,1,0,0,0") ==
                      MODEM_PROVISION_LINE_MISMATCH &&
                  dviext->parse_readback("#DVIEXT: 1,1,0,0") ==
                      MODEM_PROVISION_LINE_INVALID,
              "DVIEXT is strictly verified as reboot-classified 16 kHz I2S");
    }
    if (dvi != NULL) {
        check(dvi->recoverable && dvi->degrade == MODEM_DEGRADE_AUDIO &&
                  dvi->prerequisites == MODEM_INIT_PREREQ_NONE &&
                  dvi->persistence == MODEM_SETTING_RUNTIME &&
                  strcmp(dvi->set_cmd, "AT#DVI=1,2,1") == 0 &&
                  dvi->parse_readback("#DVI: 1,2,1") ==
                      MODEM_PROVISION_LINE_MATCH &&
                  dvi->parse_readback("#DVI: 0,2,1") ==
                      MODEM_PROVISION_LINE_MISMATCH &&
                  dvi->parse_readback("#DVI: 1,x,1") ==
                      MODEM_PROVISION_LINE_INVALID,
              "DVI runtime mode is strictly verified as enabled module-master");
    }
    if (wkio != NULL) {
        check(wkio->prerequisites ==
                      (MODEM_INIT_PREREQ_SIM_READY |
                       MODEM_INIT_PREREQ_SIM_COMPLETION) &&
                  wkio->persistence == MODEM_SETTING_NVM &&
                  !wkio->set_each_pass &&
                  strcmp(wkio->set_cmd,
                         "AT#WKIO=0;#E2SMSRI=0;\\R2&W0") == 0 &&
                  wkio->parse_readback("#WKIO: 0,0,2,1") ==
                      MODEM_PROVISION_LINE_MATCH &&
                  wkio->parse_readback("#WKIO: 1,0,2,1") ==
                      MODEM_PROVISION_LINE_MISMATCH &&
                  wkio->parse_readback("#WKIO: 1,0,4,1") ==
                      MODEM_PROVISION_LINE_INVALID,
              "event-specific WKIO wake is disabled before CFUN=5");
    }
    if (ring_profile != NULL) {
        check(ring_profile->prerequisites ==
                      (MODEM_INIT_PREREQ_SIM_READY |
                       MODEM_INIT_PREREQ_SIM_COMPLETION) &&
                  ring_profile->persistence == MODEM_SETTING_NVM &&
                  !ring_profile->set_each_pass &&
                  strcmp(ring_profile->set_cmd,
                         "AT#WKIO=0;#E2SMSRI=0;\\R2&W0") == 0 &&
                  ring_profile->parse_readback(
                      "RI (C125) OPTIONS : \\R2=follows ring") ==
                      MODEM_PROVISION_LINE_MATCH &&
                  ring_profile->parse_readback(
                      "RI (C125) OPTIONS : \\R1=follows ring") ==
                      MODEM_PROVISION_LINE_MISMATCH &&
                  ring_profile->parse_readback(
                      "RI (C125) OPTIONS : \\R9=invalid") ==
                      MODEM_PROVISION_LINE_INVALID &&
                  ring_profile->parse_readback(
                      "RI (C125) OPTIONS : malformed") ==
                      MODEM_PROVISION_LINE_INVALID &&
                  ring_profile->parse_readback("DTR (C108) OPTIONS") ==
                      MODEM_PROVISION_LINE_IGNORE,
              "ordinary call RI profile is preserved beside generic SMS wake");
    }
    if (e2smsri != NULL) {
        check(e2smsri->prerequisites ==
                      (MODEM_INIT_PREREQ_SIM_READY |
                       MODEM_INIT_PREREQ_SIM_COMPLETION) &&
                  e2smsri->persistence == MODEM_SETTING_NVM &&
                  !e2smsri->set_each_pass &&
                  strcmp(e2smsri->set_cmd,
                         "AT#WKIO=0;#E2SMSRI=0;\\R2&W0") == 0 &&
                  e2smsri->parse_readback("#E2SMSRI: 0") ==
                      MODEM_PROVISION_LINE_MATCH &&
                  e2smsri->parse_readback("#E2SMSRI: 1000") ==
                      MODEM_PROVISION_LINE_MISMATCH &&
                  e2smsri->parse_readback("#E2SMSRI: 1151") ==
                      MODEM_PROVISION_LINE_INVALID,
              "event-specific SMS RI is disabled before generic sleep wake");
    }
    if (psmri != NULL) {
        check(psmri->prerequisites ==
                      (MODEM_INIT_PREREQ_SIM_READY |
                       MODEM_INIT_PREREQ_SIM_COMPLETION) &&
                  psmri->persistence == MODEM_SETTING_PROFILE &&
                  psmri->set_each_pass &&
                  strcmp(psmri->set_cmd, "AT#PSMRI=1000") == 0 &&
                  psmri->parse_readback("#PSMRI: 1000") ==
                      MODEM_PROVISION_LINE_MATCH &&
                  psmri->parse_readback("#PSMRI: 0") ==
                      MODEM_PROVISION_LINE_MISMATCH &&
                  psmri->parse_readback("#PSMRI: 1151") ==
                      MODEM_PROVISION_LINE_INVALID,
              "generic SMS RI is re-applied without NVM save before CFUN=5");
    }
    check(cfun != NULL &&
              cfun->prerequisites == MODEM_INIT_PREREQ_NONE &&
              !cfun->qualifies_sms_wake &&
              cfun_completion == &g_modem_vendor.provision_steps[
                  g_modem_vendor.provision_step_count - 1u] &&
              cfun_completion->prerequisites ==
                  (MODEM_INIT_PREREQ_SIM_READY |
                   MODEM_INIT_PREREQ_SIM_COMPLETION) &&
              cfun_completion->qualifies_sms_wake,
          "only the focused final CFUN=5 row qualifies SMS wake");
    check(g_modem_vendor.provision_reboot_cmd != NULL &&
              strcmp(g_modem_vendor.provision_reboot_cmd, "AT#REBOOT") == 0 &&
              g_modem_vendor.provision_reboot_timeout_ms == 10000u,
          "provisioning exports one bounded reboot command");
}

static void test_call_builders(void) {
    (void)build_call(CALL_TXN_DIAL, "+14155550123", 0u,
                     "ATD+14155550123;", 30000u);
    (void)build_call(CALL_TXN_ANSWER, NULL, 0u, "ATA", 15000u);
    (void)build_call(CALL_TXN_HOLD, NULL, 0u, "AT+CHLD=2", 25000u);
    (void)build_call(CALL_TXN_SWAP, NULL, 0u, "AT+CHLD=2", 25000u);
    (void)build_call(CALL_TXN_WAIT_ANSWER, NULL, 0u,
                     "AT+CHLD=2", 25000u);
    (void)build_call(CALL_TXN_WAIT_REJECT, NULL, 0u,
                     "AT+CHLD=0", 25000u);
    (void)build_call(CALL_TXN_RELEASE_ACTIVE, NULL, 0u,
                     "AT+CHLD=1", 25000u);
    (void)build_call(CALL_TXN_RELEASE_LEG, NULL, 7u,
                     "AT+CHLD=17", 25000u);
    (void)build_call(CALL_TXN_HANGUP, NULL, 0u, "AT+CHUP", 10000u);

    char command[32];
    uint32_t timeout = 0u;
    check(!g_modem_vendor.call.build_command(
              CALL_TXN_NONE, NULL, 0u, command, sizeof(command), &timeout),
          "NONE operation rejected");
    check(!g_modem_vendor.call.build_command(
              CALL_TXN_DIAL, "123\rAT#SHDN", 0u, command,
              sizeof(command), &timeout),
          "dial command injection rejected");
    check(!g_modem_vendor.call.build_command(
              CALL_TXN_DIAL, "12;34", 0u, command,
              sizeof(command), &timeout),
          "embedded dial terminator rejected");
    check(!g_modem_vendor.call.build_command(
              CALL_TXN_RELEASE_LEG, NULL, 0u, command,
              sizeof(command), &timeout),
          "zero target rejected");
    check(!g_modem_vendor.call.build_command(
              CALL_TXN_RELEASE_LEG, NULL, MODEM_CALL_ID_MAX + 1u,
              command, sizeof(command), &timeout),
          "out-of-domain target rejected");
    check(!g_modem_vendor.call.build_command(
              CALL_TXN_DIAL, "12345", 0u, command, 4u, &timeout),
          "short output buffer rejected");

    check(g_modem_vendor.call.build_dtmf_command(
              '5', command, sizeof(command), &timeout) &&
              strcmp(command, "AT+VTS=5") == 0 && timeout == 1500u,
          "DTMF builder owns the Telit digit command and deadline");
    check(g_modem_vendor.call.build_dtmf_command(
              '*', command, sizeof(command), &timeout) &&
              strcmp(command, "AT+VTS=*") == 0,
          "DTMF builder accepts the Nokia star key");
    check(g_modem_vendor.call.build_dtmf_command(
              '#', command, sizeof(command), &timeout) &&
              strcmp(command, "AT+VTS=#") == 0,
          "DTMF builder accepts the Nokia hash key");
    check(!g_modem_vendor.call.build_dtmf_command(
              'A', command, sizeof(command), &timeout) &&
              !g_modem_vendor.call.build_dtmf_command(
                  '1', command, 4u, &timeout),
          "DTMF builder rejects hidden keys and truncated output");
}

static void test_init_table_direct_sms_delivery(void) {
    bool saw_csdh = false;
    bool saw_cnmi_direct = false;
    bool saw_cnmi_store = false;
    for (size_t i = 0u; i < TELIT_INIT_STEP_COUNT; i++) {
        const char *cmd = TELIT_INIT_STEPS[i].cmd;
        if (strcmp(cmd, "AT+CSDH=1") == 0) {
            saw_csdh = TELIT_INIT_STEPS[i].persistence == MODEM_SETTING_PROFILE;
        } else if (strcmp(cmd, "AT+CNMI=2,2,0,0,0") == 0) {
            saw_cnmi_direct = true;
        } else if (strncmp(cmd, "AT+CNMI=1,", 10u) == 0) {
            saw_cnmi_store = true;
        }
    }
    check(saw_csdh, "init sets the extended text header (+CSDH=1)");
    check(saw_cnmi_direct && !saw_cnmi_store,
          "init routes SMS-DELIVER directly (+CNMI=2,2) instead of storing");
}

static void test_antenna_tuner_policy(void) {
    check(telit_init_parse_stune_capability(
              "#STUNEANT: (0,1),(7FD9FFFF),(0,1),(0,1)"),
          "WWX STUNEANT capability is parsed");
    check(telit_tune_supported_mask() == UINT64_C(0x7FD9FFFF),
          "WWX supported mask is retained exactly");
    check(telit_tune_effective_mask(TELIT_TUNE_RF1) ==
              UINT64_C(0x601840A7) &&
              telit_tune_effective_mask(TELIT_TUNE_RF2) ==
                  UINT64_C(0x00010300) &&
              telit_tune_effective_mask(TELIT_TUNE_RF3) ==
                  UINT64_C(0x1F80BC50) &&
              telit_tune_effective_mask(TELIT_TUNE_RF4) ==
                  UINT64_C(0x00400008),
          "WWX policy is disjoint, exhaustive, and assigns B7 fallback to RF1");

    uint64_t all = 0u;
    for (telit_tune_rf_t rf = TELIT_TUNE_RF1;
         rf < TELIT_TUNE_RF_COUNT; rf++) {
        uint64_t mask = telit_tune_effective_mask(rf);
        check((all & mask) == 0u, "effective tuner masks do not overlap");
        all |= mask;
    }
    check(all == telit_tune_supported_mask(),
          "effective tuner masks cover every supported WWX bit");

    char command[MODEM_PROVISION_COMMAND_MAX];
    check(telit_tune_build_rf1(command, sizeof(command)),
          "WWX RF1 command builds");
    check_string(command, "AT#STUNEANT=1,601840A7,0,0",
                 "WWX RF1 command and CTRL ordering");
    check(telit_tune_build_rf2(command, sizeof(command)),
          "WWX RF2 command builds");
    check_string(command, "AT#STUNEANT=1,10300,0,1",
                 "WWX RF2 command and CTRL ordering");
    check(telit_tune_build_rf3(command, sizeof(command)),
          "WWX RF3 command builds");
    check_string(command, "AT#STUNEANT=1,1F80BC50,1,0",
                 "WWX RF3 command and CTRL ordering");
    check(telit_tune_build_rf4(command, sizeof(command)),
          "WWX RF4 command builds");
    check_string(command, "AT#STUNEANT=1,400008,1,1",
                 "WWX RF4 command and CTRL ordering");

    check(telit_init_parse_stune_capability(
              "#STUNEANT: (0,1),(D6071A),(0,1),(0,1)"),
          "NF STUNEANT capability is parsed");
    check(telit_tune_effective_mask(TELIT_TUNE_RF1) ==
              UINT64_C(0x00100002) &&
              telit_tune_effective_mask(TELIT_TUNE_RF2) ==
                  UINT64_C(0x00040300) &&
              telit_tune_effective_mask(TELIT_TUNE_RF3) ==
                  UINT64_C(0x00800410) &&
              telit_tune_effective_mask(TELIT_TUNE_RF4) ==
                  UINT64_C(0x00420008),
          "NF capability intersection activates B66/B71 assignments");

    telit_tune_readback_begin();
    check(telit_provision_gtune_line("#GTUNEANT: 100002,0,0") ==
              MODEM_PROVISION_LINE_IGNORE &&
              telit_provision_gtune_line("#GTUNEANT: 40300,0,1") ==
                  MODEM_PROVISION_LINE_IGNORE &&
              telit_provision_gtune_line("#GTUNEANT: 800410,1,0") ==
                  MODEM_PROVISION_LINE_IGNORE &&
              telit_provision_gtune_line("#GTUNEANT: 420008,1,1") ==
                  MODEM_PROVISION_LINE_IGNORE &&
              telit_tune_final_finish(true, false) ==
                  MODEM_PROVISION_LINE_MATCH,
          "strict multi-line readback accepts the exact NF table");

    telit_tune_readback_begin();
    (void)telit_provision_gtune_line("#GTUNEANT: 100002,0,0");
    (void)telit_provision_gtune_line("#GTUNEANT: 40300,0,1");
    (void)telit_provision_gtune_line("#GTUNEANT: 800410,1,0");
    (void)telit_provision_gtune_line("#GTUNEANT: 420008,1,0");
    check(telit_tune_final_finish(true, false) ==
              MODEM_PROVISION_LINE_MISMATCH,
          "strict readback rejects a band assigned to the wrong throw");

    telit_tune_readback_begin();
    check(telit_tune_discovery_finish(false, false) ==
              MODEM_PROVISION_LINE_MATCH &&
              telit_tune_repair_required(),
          "factory-reset tuner query error schedules bounded repair");
    check(telit_tune_final_finish(false, false) ==
              MODEM_PROVISION_LINE_INVALID,
          "unreadable tuner table never passes final verification");
    check(telit_tune_discovery_finish(false, true) ==
              MODEM_PROVISION_LINE_INVALID,
          "tuner discovery timeout is not treated as a reset table");

    telit_tune_readback_begin();
    (void)telit_provision_gtune_line("#GTUNEANT: 100002,0,0");
    check(telit_tune_discovery_finish(false, false) ==
              MODEM_PROVISION_LINE_INVALID,
          "partial tuner reply followed by error remains invalid");
    check(telit_tune_discovery_finish(true, false) ==
              MODEM_PROVISION_LINE_MATCH &&
              telit_tune_repair_required(),
          "interrupted partial table is classified for repair");

    telit_tune_readback_begin();
    (void)telit_provision_gtune_line("#GTUNEANT: 0,1,1");
    (void)telit_provision_gtune_line("#GTUNEANT: 0,1,0");
    check(telit_tune_discovery_finish(true, false) ==
              MODEM_PROVISION_LINE_MATCH &&
              telit_tune_repair_required() &&
              telit_tune_row_finish(true, false) ==
                  MODEM_PROVISION_LINE_MISMATCH,
          "WWX zero-mask placeholders are repairable but never verify");

    check(telit_init_parse_stune_capability(
              "#STUNEANT: (0,1),(7FD9FFFF),(0,1),(0,1)"),
          "WWX capability is restored for incremental-row verification");
    telit_tune_readback_target_rf2();
    (void)telit_provision_gtune_line("#GTUNEANT: 0,1,1");
    (void)telit_provision_gtune_line("#GTUNEANT: 0,1,0");
    (void)telit_provision_gtune_line("#GTUNEANT: 10300,0,1");
    (void)telit_provision_gtune_line("#GTUNEANT: 7FD8FCFF,0,0");
    check(telit_tune_row_finish(true, false) ==
              MODEM_PROVISION_LINE_MATCH,
          "bench-observed first RF2 write verifies before the table is final");
    check(telit_tune_final_finish(true, false) ==
              MODEM_PROVISION_LINE_INVALID,
          "the same intermediate table cannot pass final verification");

    telit_tune_readback_begin();
    (void)telit_provision_gtune_line("#GTUNEANT: 100002,0,0");
    (void)telit_provision_gtune_line("#GTUNEANT: 2,1,0");
    check(telit_tune_discovery_finish(true, false) ==
              MODEM_PROVISION_LINE_INVALID,
          "overlapping table rows fail closed instead of being rewritten blindly");
    check(!telit_init_parse_stune_capability(
              "#STUNEANT: (0,1),(1000000000),(0,1),(0,1)"),
          "capability parser rejects masks outside Telit's command domain");
}

static void test_ecam_mapping(void) {
    static const modem_call_event_kind_t expected[10] = {
        MODEM_CALL_EV_RELEASED,
        MODEM_CALL_EV_DIALING,
        MODEM_CALL_EV_ALERTING_MO,
        MODEM_CALL_EV_ACTIVE,
        MODEM_CALL_EV_HELD,
        MODEM_CALL_EV_WAITING_MT,
        MODEM_CALL_EV_RINGING_MT,
        MODEM_CALL_EV_BUSY,
        MODEM_CALL_EV_ACTIVE,
        MODEM_CALL_EV_SETUP_DONE,
    };

    for (unsigned status = 0u; status <= 9u; status++) {
        char line[80];
        snprintf(line, sizeof(line), "#ECAM: 0,%u,1,,,", status);
        modem_call_event_t event = {
            .call_id = 99u,
            .id_valid = true,
            .event = MODEM_CALL_EV_BUSY,
        };
        check(g_modem_vendor.parse_call_urc(line, &event),
              "ECAM status accepted");
        check(event.event == expected[status], "ECAM status mapping");
        check(!event.id_valid && event.call_id == 0u,
              "manual ccid zero remains coarse");
    }

    modem_call_event_t event;
    check(g_modem_vendor.parse_call_urc(
              "#ECAM: 0,1,1,,,\"0YYYYYYYYY\",129", &event),
          "manual ECAM dial example");
    check(event.event == MODEM_CALL_EV_DIALING,
          "manual dial example maps to dialing");
    check(g_modem_vendor.parse_call_urc(
              "#ECAM: 7,3,1,,,", &event),
          "nonzero ECAM ID accepted as coarse evidence");
    check(event.call_id == 7u && !event.id_valid,
          "nonzero ECAM ID preserved but untrusted");
    check(g_modem_vendor.parse_call_urc(
              "#ECAM: 8,3,1,,,", &event),
          "out-of-table ECAM ID accepted as coarse evidence");
    check(event.call_id == 0u && !event.id_valid,
          "out-of-table ECAM ID cannot address a leg");

    event.call_id = 55u;
    event.id_valid = true;
    event.event = MODEM_CALL_EV_HELD;
    static const char *const malformed[] = {
        "#ECAM: 1",
        "#ECAM: 0,10,1,,,",
        "#ECAM: 0,3,0,,,",
        "#ECAM: 0,3,1,x,,",
        "#ECAM: 0,3,1,,,\"unterminated,129",
        "#ECAM: 0,3,1,,,123",
        "#ECAM: 0,3,1,,,123,x",
        "#ECAM: 0,3,1,,,\"123\",129",
        "#ECAM: 0,1,1,,,\"123\",130",
        "#ECAM: 0,3,1,,,,129",
        "#ECAM: 0,3,1,,,\r,129",
    };
    for (size_t i = 0u; i < sizeof(malformed) / sizeof(malformed[0]); i++) {
        check(!g_modem_vendor.parse_call_urc(malformed[i], &event),
              "malformed ECAM rejected");
        check(event.call_id == 55u && event.id_valid &&
                  event.event == MODEM_CALL_EV_HELD,
              "failed ECAM parse is atomic");
    }

    modem_clcc_row_t row;
    check(g_modem_vendor.parse_clcc_row(
              "+CLCC: 3,1,5,0,1,\"+14155550123\",145", &row),
          "shared strict CLCC parser accepts Telit row");
    check(row.id == 3u && row.dir == CALL_DIR_MT &&
              row.state == CALL_LEG_WAITING &&
              row.mode == CALL_MODE_VOICE && row.mpty,
          "CLCC row normalized");
    check_string(row.number, "+14155550123", "CLCC number");
}

static void test_sim_mwi_temperature_and_readbacks(void) {
    telit_qss_state_t qss = {0};
    check(telit_parse_qss_query("#QSS: 2,3", &qss),
          "QSS query parsed");
    check(qss.mode == 2u && qss.status == 3u,
          "QSS mode and ready status");
    check(telit_parse_qss_urc("#QSS: 0", &qss.status),
          "QSS missing-SIM URC parsed");
    check(qss.mode == 2u && qss.status == 0u,
          "QSS URC updates status only");
    check(!telit_parse_qss_urc("#QSS: 2,3", &qss.status),
          "QSS query shape rejected as URC");
    check(qss.mode == 2u && qss.status == 0u,
          "failed QSS URC parse leaves typed state unchanged");

    telit_mwi_state_t mwi = {0};
    check(modem_message_waiting_category_bit(MODEM_MESSAGE_WAITING_ALL) == 0u,
          "message-waiting all-category sentinel is never an array bit");
    check(telit_parse_mwi_query("#MWI: 1,1,1,3", &mwi),
          "MWI query parsed");
    check(mwi.enabled == 1u &&
              mwi.category == MODEM_MESSAGE_WAITING_VOICE_LINE_1 &&
              mwi.active && mwi.count == 3u,
          "voice MWI query state");
    check(telit_parse_mwi_urc("#MWI: 1,3,8", &mwi),
          "non-voice MWI parsed");
    check(mwi.category == MODEM_MESSAGE_WAITING_FAX && mwi.active &&
              mwi.count == 8u,
          "fax MWI maps to a neutral independent category");
    check(telit_parse_mwi_urc("#MWI: 0,1", &mwi),
          "voice MWI clear parsed");
    check(mwi.category == MODEM_MESSAGE_WAITING_VOICE_LINE_1 &&
              !mwi.active && mwi.count == 0u,
          "voice MWI cleared");
    check(!telit_parse_mwi_urc("#MWI: 1", &mwi),
          "set MWI requires an indicator");
    check(mwi.category == MODEM_MESSAGE_WAITING_VOICE_LINE_1 &&
              !mwi.active && mwi.count == 0u,
          "failed MWI URC parse leaves typed state unchanged");
    check(telit_parse_mwi_query("#MWI: 1,1,3,2", &mwi),
          "MWI query can report only a non-voice indicator");
    check(mwi.category == MODEM_MESSAGE_WAITING_FAX && mwi.active &&
              mwi.count == 2u,
          "non-voice MWI query preserves its category and count");
    modem_aux_event_t mwi_row;
    check(telit_parse_message_waiting_row("#MWI: 1,1,1,7", &mwi_row) ==
              MODEM_MESSAGE_WAITING_ROW_VALID &&
              mwi_row.kind == MODEM_AUX_EVENT_MESSAGE_WAITING &&
              mwi_row.message_waiting_category ==
                  MODEM_MESSAGE_WAITING_VOICE_LINE_1 &&
              mwi_row.active && mwi_row.count == 7u,
          "typed MWI read row exposes stored voice state");
    check(telit_parse_message_waiting_row("#MWI: 1,0", &mwi_row) ==
              MODEM_MESSAGE_WAITING_ROW_VALID &&
              mwi_row.kind == MODEM_AUX_EVENT_MESSAGE_WAITING &&
              mwi_row.message_waiting_category == MODEM_MESSAGE_WAITING_ALL &&
              !mwi_row.active && mwi_row.count == 0u,
          "typed MWI read row exposes global clear");
    check(telit_parse_message_waiting_row("#MWI: 1,1,3,2", &mwi_row) ==
              MODEM_MESSAGE_WAITING_ROW_VALID &&
              mwi_row.kind == MODEM_AUX_EVENT_MESSAGE_WAITING &&
              mwi_row.message_waiting_category == MODEM_MESSAGE_WAITING_FAX &&
              mwi_row.active && mwi_row.count == 2u,
          "typed MWI read row exposes fax state");
    check(telit_parse_message_waiting_row("#MWI: 1,1,4,5", &mwi_row) ==
              MODEM_MESSAGE_WAITING_ROW_VALID &&
              mwi_row.message_waiting_category ==
                  MODEM_MESSAGE_WAITING_EMAIL &&
              mwi_row.active && mwi_row.count == 5u,
          "typed MWI read row exposes e-mail state");
    check(telit_parse_message_waiting_row("#MWI: 1,1", &mwi_row) ==
              MODEM_MESSAGE_WAITING_ROW_AMBIGUOUS &&
              mwi_row.message_waiting_uncertain_mask ==
                  modem_message_waiting_category_bit(
                      MODEM_MESSAGE_WAITING_VOICE_LINE_1),
          "two-field live WWX readback remains non-authoritative");
    check(telit_parse_message_waiting_row("#MWI: 1,1,3", &mwi_row) ==
              MODEM_MESSAGE_WAITING_ROW_AMBIGUOUS &&
              mwi_row.message_waiting_uncertain_mask ==
                  (modem_message_waiting_category_bit(
                       MODEM_MESSAGE_WAITING_VOICE_LINE_1) |
                   modem_message_waiting_category_bit(
                       MODEM_MESSAGE_WAITING_FAX)),
          "three-field read/URC overlap remains non-authoritative");
    check(telit_parse_message_waiting_row("#MWI: 1,1,7", &mwi_row) ==
              MODEM_MESSAGE_WAITING_ROW_INVALID,
          "three-field voice-count seven is not mistaken for a read row");
    check(telit_parse_message_waiting_row("#MWI: 1,1,0", &mwi_row) ==
              MODEM_MESSAGE_WAITING_ROW_INVALID,
          "typed MWI read row rejects an invalid indicator");

    telit_temperature_t temperature = {0};
    check(telit_parse_temperature("#TEMPMEAS: -1,-32", &temperature),
          "temperature line parsed");
    check(temperature.level == -1 && temperature.celsius == -32,
          "signed temperature retained");
    check(!telit_parse_temperature("#TEMPMEAS: 3,25", &temperature),
          "temperature level bounds enforced");
    check(temperature.level == -1 && temperature.celsius == -32,
          "failed temperature parse leaves typed state unchanged");

    uint8_t ismscfg = 0u;
    telit_fwswitch_t fwswitch = {0};
    uint8_t fwautosim = 0u;
    check(telit_parse_ismscfg("#ISMSCFG: 1", &ismscfg),
          "ISMSCFG readback");
    check(telit_parse_fwswitch("#FWSWITCH: 0,0,1", &fwswitch),
          "FWSWITCH readback");
    check(telit_parse_fwautosim("#FWAUTOSIM: 2", &fwautosim),
          "FWAUTOSIM readback");
    check(ismscfg == 1u && fwswitch.image == 0u &&
              fwswitch.storage == 0u && fwswitch.restore == 1u &&
              fwautosim == 2u,
          "setting readbacks stored");

    check(!telit_parse_fwswitch("#FWSWITCH: 0,2,0", &fwswitch),
          "FWSWITCH storage bound enforced");
    check(!telit_parse_fwautosim("#FWAUTOSIM: 4", &fwautosim),
          "FWAUTOSIM bound enforced");
    check(fwswitch.image == 0u && fwswitch.storage == 0u &&
              fwswitch.restore == 1u && fwautosim == 2u,
          "failed firmware readbacks leave typed state unchanged");

    modem_aux_event_t aux;
    check(g_modem_vendor.parse_aux_urc("#QSS: 3", &aux) &&
              aux.kind == MODEM_AUX_EVENT_NONE,
          "aux dispatcher handles QSS");
    check(g_modem_vendor.parse_aux_urc("#MWI: 1,1,2", &aux) &&
              aux.kind == MODEM_AUX_EVENT_MESSAGE_WAITING &&
              aux.message_waiting_category ==
                  MODEM_MESSAGE_WAITING_VOICE_LINE_1 &&
              aux.active && aux.count == 2u,
          "aux dispatcher handles MWI");
    check(g_modem_vendor.parse_aux_urc("#MWI: 1,4,6", &aux) &&
              aux.kind == MODEM_AUX_EVENT_MESSAGE_WAITING &&
              aux.message_waiting_category == MODEM_MESSAGE_WAITING_EMAIL &&
              aux.active && aux.count == 6u,
          "aux dispatcher normalizes e-mail MWI");
    check(g_modem_vendor.parse_aux_urc("#CFF: 1,1,+15551212", &aux) &&
              aux.kind == MODEM_AUX_EVENT_CFU_STATE && aux.active &&
              strcmp(aux.number, "+15551212") == 0,
          "aux dispatcher normalizes CFU state");
    check(g_modem_vendor.parse_aux_urc("#CFF: 1", &aux) &&
              aux.kind == MODEM_AUX_EVENT_NONE &&
              g_modem_vendor.parse_aux_urc("#CFF: 0", &aux) &&
              aux.kind == MODEM_AUX_EVENT_NONE,
          "configuration-only CFF readback does not invent a forwarding flag");
    check(!g_modem_vendor.parse_aux_urc("#CFF: 1,9,", &aux) &&
              !g_modem_vendor.parse_aux_urc("#CFF: garbage", &aux),
          "malformed local forwarding flags are rejected");
    check(g_modem_vendor.parse_aux_urc("+CSSU: 0", &aux) &&
              aux.kind == MODEM_AUX_EVENT_INCOMING_DIVERTED,
          "aux dispatcher normalizes redirected MT call");
    check(g_modem_vendor.parse_aux_urc("+CSSU: 10", &aux) &&
              aux.kind == MODEM_AUX_EVENT_INCOMING_DIVERTED,
          "aux dispatcher normalizes additional redirected MT call");
    check(g_modem_vendor.parse_aux_urc("#TEMPMEAS: 0,28", &aux) &&
              aux.kind == MODEM_AUX_EVENT_NONE,
          "aux dispatcher handles temperature");
    /* The Qualcomm store indication: a message filed where this image cannot
     * read it. Neutral kind; the generic service never sees the URC name. */
    bool qcmti_known = false;
    for (uint8_t i = 0u; i < g_modem_vendor.aux_urc_prefix_count; i++) {
        if (strcmp(g_modem_vendor.aux_urc_prefixes[i], "$QCMTI:") == 0) {
            qcmti_known = true;
        }
    }
    check(qcmti_known, "$QCMTI: is a vendor aux URC prefix");
    check(g_modem_vendor.parse_aux_urc("$QCMTI: \"ME\",24", &aux) &&
              aux.kind == MODEM_AUX_EVENT_MESSAGE_STORED_UNREADABLE,
          "$QCMTI maps to the unreadable-store event");
    check(!g_modem_vendor.parse_aux_urc("$QCMTI: \"ME\"", &aux) &&
              !g_modem_vendor.parse_aux_urc("$QCMTI: \"ME\",x", &aux),
          "malformed $QCMTI is rejected");
    check(!g_modem_vendor.parse_aux_urc("#UNKNOWN: 1", &aux),
          "aux dispatcher rejects unknown prefix");
}

static void test_call_forwarding_and_mailbox(void) {
    char command[96];
    call_forward_request_t request = {
        .reason = CALL_FORWARD_REASON_UNCONDITIONAL,
        .action = CALL_FORWARD_ACTION_REGISTER,
        .has_number = true,
    };
    strcpy(request.number, "+15551234567");
    check(telit_call_forward_step_count(&request) == 1u &&
              telit_build_call_forward_step(&request, 0u, command,
                                            sizeof(command)),
          "build unconditional registration");
    check_string(command, "AT+CCFC=0,3,\"+15551234567\",145,1",
                 "unconditional registration syntax");

    request.action = CALL_FORWARD_ACTION_ENABLE;
    check(telit_call_forward_step_count(&request) == 1u &&
              telit_build_call_forward_step(&request, 0u, command,
                                            sizeof(command)),
          "build activation with destination");
    check_string(command, "AT+CCFC=0,1,\"+15551234567\",145,1",
                 "activation with destination syntax");

    request.has_number = false;
    request.number[0] = '\0';
    request.reason = CALL_FORWARD_REASON_NO_REPLY;
    request.has_delay = true;
    request.delay_seconds = 20u;
    check(telit_call_forward_step_count(&request) == 1u &&
              telit_build_call_forward_step(&request, 0u, command,
                                            sizeof(command)),
          "build timer-only no-reply activation");
    check_string(command, "AT+CCFC=2,1,,,1,20",
                 "timer-only activation reuses registered destination");

    request.reason = CALL_FORWARD_REASON_NO_REPLY;
    request.action = CALL_FORWARD_ACTION_REGISTER;
    request.has_number = true;
    strcpy(request.number, "+15551234567");
    request.has_delay = true;
    request.delay_seconds = 20u;
    check(telit_call_forward_step_count(&request) == 2u &&
              telit_build_call_forward_step(&request, 0u, command,
                                            sizeof(command)),
          "build first no-reply registration step");
    check_string(command, "AT+CCFC=2,3,\"+15551234567\",145,1",
                 "no-reply registration omits undocumented timer");
    check(telit_build_call_forward_step(&request, 1u, command,
                                        sizeof(command)),
          "build second no-reply registration step");
    check_string(command, "AT+CCFC=2,1,,,1,20",
                 "documented no-reply enable carries timer");

    request.reason = CALL_FORWARD_REASON_ALL_CONDITIONAL;
    check(telit_call_forward_step_count(&request) == 2u &&
              telit_build_call_forward_step(&request, 0u, command,
                                            sizeof(command)),
          "build all-conditional registration first step");
    check_string(command, "AT+CCFC=5,3,\"+15551234567\",145,1",
                 "all-conditional registration omits timer");
    check(telit_build_call_forward_step(&request, 1u, command,
                                        sizeof(command)),
          "build all-conditional timer follow-up");
    check_string(command, "AT+CCFC=2,1,,,1,20",
                 "all-conditional delay targets no-reply component");
    request.action = CALL_FORWARD_ACTION_ENABLE;
    check(telit_call_forward_step_count(&request) == 0u,
          "all-conditional activation cannot carry Telit no-reply timer");

    memset(&request, 0, sizeof(request));
    request.reason = CALL_FORWARD_REASON_BUSY;
    request.action = CALL_FORWARD_ACTION_QUERY;
    check(telit_call_forward_step_count(&request) == 1u &&
              telit_build_call_forward_step(&request, 0u, command,
                                            sizeof(command)),
          "build voice-only query");
    check_string(command, "AT+CCFC=1,2,,,1", "voice query syntax");

    request.reason = CALL_FORWARD_REASON_ALL;
    request.action = CALL_FORWARD_ACTION_ERASE;
    check(telit_call_forward_step_count(&request) == 1u &&
              telit_build_call_forward_step(&request, 0u, command,
                                            sizeof(command)),
          "build Nokia cancel-all");
    check_string(command, "AT+CCFC=4,4", "cancel-all omits class");

    request.action = CALL_FORWARD_ACTION_DISABLE;
    check(telit_call_forward_step_count(&request) == 1u &&
              telit_build_call_forward_step(&request, 0u, command,
                                            sizeof(command)),
          "build standard deactivate-all");
    check_string(command, "AT+CCFC=4,0,,,1",
                 "deactivate-all remains voice-qualified");

    request.action = CALL_FORWARD_ACTION_ENABLE;
    check(telit_call_forward_step_count(&request) == 1u &&
              telit_build_call_forward_step(&request, 0u, command,
                                            sizeof(command)),
          "build standard activate-all");
    check_string(command, "AT+CCFC=4,1,,,1",
                 "activate-all reuses registered destinations");

    request.action = CALL_FORWARD_ACTION_REGISTER;
    request.has_number = true;
    strcpy(request.number, "+15551234567");
    check(telit_call_forward_step_count(&request) == 1u &&
              telit_build_call_forward_step(&request, 0u, command,
                                            sizeof(command)),
          "build standard register-all");
    check_string(command, "AT+CCFC=4,3,\"+15551234567\",145,1",
                 "register-all carries the voice destination");

    request.action = CALL_FORWARD_ACTION_QUERY;
    request.has_number = false;
    request.number[0] = '\0';
    check(telit_call_forward_step_count(&request) == 0u,
          "Telit does not query aggregate all-call reason");

    memset(&request, 0, sizeof(request));
    request.reason = CALL_FORWARD_REASON_UNCONDITIONAL;
    request.action = CALL_FORWARD_ACTION_REGISTER;
    request.has_number = true;
    memset(request.number, '1', sizeof(request.number));
    check(telit_call_forward_step_count(&request) == 0u,
          "unterminated forwarding number is rejected without overread");
    memset(&request, 0, sizeof(request));
    request.reason = (call_forward_reason_t)-1;
    request.action = CALL_FORWARD_ACTION_QUERY;
    check(telit_call_forward_step_count(&request) == 0u,
          "negative forwarding enum cannot reach a shift or command builder");

    call_forward_row_t row;
    check(telit_parse_call_forward_row(
              "+CCFC: 1,1,\"+15551234567\",145,,,20", &row) &&
              row.active && row.class_mask == 1u && row.has_number &&
              row.has_delay && row.delay_seconds == 20u,
          "active voice forwarding row parsed");
    check(telit_parse_call_forward_row("+CCFC: 0,7", &row) &&
              !row.active && (row.class_mask & 1u) != 0u,
          "inactive aggregate row parsed");
    check(!telit_parse_call_forward_row(
              "+CCFC: 1,1,\"+1555\",129", &row),
          "international number/type mismatch rejected");

    modem_voice_mailbox_row_t mailbox;
    check(telit_parse_voice_mailbox_row(
              "#MBN: 1,\"+18005551212\",145,\"Voice mail\",VOICE",
              &mailbox) && mailbox.voice &&
              strcmp(mailbox.number, "+18005551212") == 0,
          "voice SIM mailbox row parsed");
    check(telit_parse_voice_mailbox_row(
              "#MBN: 2,\"5551213\",129,\"Fax\",FAX", &mailbox) &&
              !mailbox.voice,
          "non-voice mailbox remains distinguishable");
    check(!telit_parse_voice_mailbox_row(
              "#MBN: 3,\"5551214\",129,\"Mystery\",BOGUS", &mailbox),
          "unknown SIM mailbox type is rejected");
}

static void test_parser_bounds(void) {
    telit_csv_view_t views[3];
    size_t count = 0u;
    check(telit_csv_view_split(" 1 ,\"two,too\",", views, 3u, &count) &&
              count == 3u && views[0].length == 1u &&
              views[1].length == 7u && views[2].length == 0u,
          "zero-copy CSV splitter trims and preserves quoted commas");
    char view_text[16];
    check(telit_view_copy_exact(view_text, sizeof(view_text), views[1]),
          "zero-copy CSV view fits destination");
    check_string(view_text, "two,too", "zero-copy quoted field");
    check(!telit_csv_view_split("\"unterminated", views, 3u, &count),
          "zero-copy CSV rejects unterminated quote");
    char oversized[80];
    memset(oversized, '7', sizeof(oversized) - 1u);
    oversized[sizeof(oversized) - 1u] = '\0';
    check(!telit_csv_view_split(oversized, views, 3u, &count),
          "zero-copy CSV rejects oversized fields");
    check(!telit_csv_view_split("1,2,3,4", views, 3u, &count),
          "zero-copy CSV rejects excess fields");

    uint32_t value = 0u;
    check(telit_view_parse_u32(
              (telit_csv_view_t){ .text = "255", .length = 3u },
              255u, &value) &&
              value == 255u,
          "bounded integer accepts maximum");
    check(!telit_view_parse_u32(
              (telit_csv_view_t){ .text = "256", .length = 3u },
              255u, &value),
          "bounded integer rejects maximum plus one");
    check(!telit_view_parse_u32(
              (telit_csv_view_t){ .text = "9", .length = 1u },
              5u, &value),
          "single oversized digit rejected without wrap");

    uint64_t hex_value = 0u;
    check(telit_view_parse_hex_u64(
              (telit_csv_view_t){ .text = "FFFFFFFF", .length = 8u },
              UINT32_MAX, &hex_value) && hex_value == UINT32_MAX,
          "target-width hex parser accepts uint32 maximum");
    check(!telit_view_parse_hex_u64(
              (telit_csv_view_t){ .text = "100000000", .length = 9u },
              UINT32_MAX, &hex_value),
          "target-width hex parser rejects uint32 maximum plus one");
    check(!telit_view_parse_hex_u64(
              (telit_csv_view_t){ .text = "9", .length = 1u },
              5u, &hex_value),
          "bounded hex parser rejects a single digit above a small maximum");
}

static void test_typed_diagnostics(void) {
    modem_diag_snapshot_t diag;
    reset_diag_snapshot(&diag);

    check(g_modem_vendor.diag_queries != NULL &&
              g_modem_vendor.diag_query_count > 30u &&
              g_modem_vendor.diag_group_finish != NULL,
          "Telit exports the typed diagnostic boundary");
    check(find_diag_query("AT#RFSTS") != NULL &&
              find_diag_query("AT#MONI") != NULL &&
              find_diag_query("AT#GTUNEANT?") != NULL &&
              find_diag_query("AT#DVIEXT?") != NULL &&
              find_diag_query("AT+CPBS?") != NULL,
          "typed table covers RF, tuner, DVI, and storage groups");
    check(find_diag_query("AT#SERVINFO") == NULL,
          "typed serving diagnostics use MONI's repeatable cell identity");
    check(strcmp(g_modem_vendor.signal_query.query_cmd, "AT#RFSTS") == 0 &&
              strcmp(g_modem_vendor.signal_query.response_prefix,
                     "#RFSTS:") == 0 &&
              g_modem_vendor.signal_query.timeout_ms == 5000u &&
              g_modem_vendor.signal_query.parse_response != NULL,
          "runtime serving sample is exposed through the neutral vendor contract");
    check(find_diag_query("AT+CGPADDR") == NULL,
          "WWX typed packet diagnostics avoid rejected parameterless CGPADDR");
    const modem_diag_query_t *context_details =
        find_diag_query("AT+CGCONTRDP");
    check(context_details != NULL && context_details->optional &&
              context_details->isolate_malformed,
          "malformed optional CGCONTRDP rows are isolated from packet basics");
    for (uint8_t i = 0u; i < g_modem_vendor.diag_query_count; i++) {
        const modem_diag_query_t *query = &g_modem_vendor.diag_queries[i];
        check(query->group > MODEM_DIAG_GROUP_NONE &&
                  query->group < MODEM_DIAG_GROUP_COUNT &&
                  query->cmd != NULL && query->timeout_ms > 0u &&
                  query->parse != NULL,
              "every typed diagnostic query is complete and bounded");
        check(strstr(query->cmd, "AT#SHDN") == NULL &&
                  strstr(query->cmd, "AT#REBOOT") == NULL &&
                  strstr(query->cmd, "AT#RXDIV=") == NULL &&
                  strstr(query->cmd, "AT#STUNEANT=0") == NULL &&
                  strstr(query->cmd, "AT#GPIO=") == NULL,
              "read-only diagnostic table contains no control command");
    }

    static const char rfsts[] =
        "#RFSTS: \"310 410\",5230,-101,-82,-11.5,00af,,32,3,1,"
        "abcdef01,\"310410123456789\",\"AT&T\",2,12,100";
    check(telit_diag_parse_rfsts(rfsts, &diag) == MODEM_DIAG_LINE_ACCEPT,
          "typed LTE RFSTS accepted");
    check(diag.serving.rat == MODEM_DIAG_RAT_LTE &&
              diag.serving.band == 12u && diag.serving.channel == 5230u &&
              diag.serving.rsrp_dbm == -101 &&
              diag.serving.rssi_dbm == -82 &&
              diag.serving.rsrq_db_x2 == -23 &&
              diag.serving.sinr_db_x10 == 0 &&
              diag.serving.inferred_rf_state == 2u &&
              diag.serving.inferred_rf_tuned,
          "typed serving metrics and RF2 policy are normalized");
    check(telit_diag_parse_moni(
              "#MONI: AT&T RSRP:-101 RSRQ:-11 TAC:00af Id:abcdef01 "
              "EARFCN:5230 PWR:-82dbm DRX:32 pci:321 QRxLevMin:-130",
              &diag) == MODEM_DIAG_LINE_ACCEPT &&
              diag.serving.pci == 321u,
          "MONI augments the exact RFSTS cell with decimal PCI");
    check(telit_diag_group_finish(MODEM_DIAG_GROUP_SERVING, &diag),
          "complete serving shadow validates");

    modem_diag_serving_t before = diag.serving;
    check(telit_diag_parse_moni(
              "#MONI: AT&T RSRP:-101 RSRQ:-11 TAC:00af Id:abcdef01 "
              "EARFCN:1025 PWR:-82dbm DRX:32 pci:342 QRxLevMin:-130",
              &diag) == MODEM_DIAG_LINE_INVALID &&
              memcmp(&before, &diag.serving, sizeof(before)) == 0,
          "MONI EARFCN mismatch invalidates the shadow atomically");
    check(telit_diag_parse_moni(
              "#MONI: AT&T RSRP:-101 RSRQ:-11 TAC:3401 Id:abcdef01 "
              "EARFCN:5230 PWR:-82dbm DRX:32 pci:342 QRxLevMin:-130",
              &diag) == MODEM_DIAG_LINE_INVALID &&
              memcmp(&before, &diag.serving, sizeof(before)) == 0,
          "MONI TAC mismatch invalidates the shadow atomically");
    check(telit_diag_parse_moni(
              "#MONI: AT&T RSRP:-101 RSRQ:-11 TAC:00af Id:abcdef02 "
              "EARFCN:5230 PWR:-82dbm DRX:32 pci:342 QRxLevMin:-130",
              &diag) == MODEM_DIAG_LINE_INVALID &&
              memcmp(&before, &diag.serving, sizeof(before)) == 0,
          "MONI cell-ID mismatch invalidates the shadow atomically");
    check(telit_diag_parse_rfsts("#RFSTS: bad", &diag) ==
              MODEM_DIAG_LINE_INVALID &&
              memcmp(&before, &diag.serving, sizeof(before)) == 0,
          "malformed typed RFSTS cannot partially overwrite its shadow");

    modem_signal_sample_t signal;
    memset(&signal, 0xa5, sizeof(signal));
    check(telit_parse_signal_response(
              "#RFSTS: \"310 410\",5230,-107,-84,-17,00af,,32,3,1,"
              "abcdef01,\"310410123456789\",\"\",2,12,106",
              &signal) &&
              signal.rat == MODEM_SIGNAL_RAT_LTE &&
              signal.rsrp_dbm == -107 && signal.rsrq_db_x2 == -34 &&
              signal.sinr_db_x10 == 12 && signal.channel == 5230u &&
              strcmp(signal.mcc, "310") == 0 &&
              strcmp(signal.mnc, "410") == 0 &&
              (signal.valid_fields & MODEM_SIGNAL_VALID_PLMN) != 0u &&
              signal.cell_id == UINT32_C(0xabcdef01) &&
              (signal.valid_fields &
               (MODEM_SIGNAL_VALID_RSRP | MODEM_SIGNAL_VALID_RSRQ |
                MODEM_SIGNAL_VALID_SINR | MODEM_SIGNAL_VALID_CHANNEL |
                MODEM_SIGNAL_VALID_CELL_ID)) ==
               (MODEM_SIGNAL_VALID_RSRP | MODEM_SIGNAL_VALID_RSRQ |
                MODEM_SIGNAL_VALID_SINR | MODEM_SIGNAL_VALID_CHANNEL |
                MODEM_SIGNAL_VALID_CELL_ID),
          "runtime projection preserves empty optionals and converts raw SINR 106 once");
    modem_signal_sample_t signal_before = signal;
    check(!telit_parse_signal_response(
              "#RFSTS: \"310 410\",5230,-107,-84,-17,00af,,32,3,1,"
              "100000000,\"310410123456789\",\"\",2,12,106",
              &signal) &&
              memcmp(&signal, &signal_before, sizeof(signal)) == 0,
          "runtime RFSTS rejects a cell id wider than the uint32 target domain");
    check(!telit_parse_signal_response("#RFSTS: bad", &signal) &&
              memcmp(&signal, &signal_before, sizeof(signal)) == 0,
          "malformed runtime RFSTS cannot partially overwrite its destination");

    reset_diag_snapshot(&diag);
    check(telit_diag_parse_cgatt("+CGATT: 1", &diag) ==
              MODEM_DIAG_LINE_ACCEPT &&
              telit_diag_parse_cgact("+CGACT: 1,1", &diag) ==
                  MODEM_DIAG_LINE_ACCEPT &&
              telit_diag_parse_cgact("+CGACT: 2,1", &diag) ==
                  MODEM_DIAG_LINE_ACCEPT &&
              telit_diag_parse_cgact("+CGACT: 3,0", &diag) ==
                  MODEM_DIAG_LINE_ACCEPT &&
              telit_diag_parse_cgact("+CGACT: 4,0", &diag) ==
                  MODEM_DIAG_LINE_ACCEPT,
          "bench packet attach and context rows parse");
    check(telit_diag_parse_cgcontrdp(
              "+CGCONTRDP: 1,5,\"apn.example\","
              "\"192.0.2.168.255.255.255.240\" "
              "\"32.1.13.184.0.18.0.52.0.0.0.0.0.0.0.1.255.255.255.255.255.255.255.255.0.0.0.0.0.0.0.0\","
              "\"192.0.2.169\" \"254.128.0.0.0.0.0.0.0.0.0.0.0.0.1.64\"",
              &diag) == MODEM_DIAG_LINE_ACCEPT &&
              telit_diag_parse_cgcontrdp(
                  "+CGCONTRDP: 2,6,\"ims\","
                  "\"32.1.13.184.0.18.0.53.0.0.0.0.0.0.0.2.255.255.255.255.255.255.255.255.0.0.0.0.0.0.0.0\","
                  "\"254.128.0.0.0.0.0.0.0.0.0.0.0.0.2.64\"",
                  &diag) == MODEM_DIAG_LINE_ACCEPT &&
              telit_diag_group_finish(MODEM_DIAG_GROUP_PACKET, &diag),
          "WWX adjacent IPv4/IPv6 CGCONTRDP fields parse atomically");
    check(diag.packet.attached == 1u &&
              diag.packet.context_count == 4u &&
              diag.packet.active_context_count == 2u &&
              diag.packet.first_cid == 1u &&
              strcmp(diag.packet.apn, "apn.example") == 0 &&
              strcmp(diag.packet.address,
                     "192.0.2.168.255.255.255.240") == 0,
          "packet page selects the first active context and its IPv4 address");

    reset_diag_snapshot(&diag);
    check(telit_diag_parse_cgcontrdp(
              "+CGCONTRDP: 1,5,apn.example", &diag) ==
                  MODEM_DIAG_LINE_ACCEPT &&
              (diag.group[MODEM_DIAG_GROUP_PACKET].present_fields &
               MODEM_DIAG_PACKET_APN) != 0u &&
              (diag.group[MODEM_DIAG_GROUP_PACKET].present_fields &
               MODEM_DIAG_PACKET_ADDRESS) == 0u &&
              strcmp(diag.packet.apn, "apn.example") == 0,
          "CGCONTRDP accepts the documented addressless minimum row");

    char max_apn[MODEM_DIAG_PACKET_APN_MAX + 1u];
    memset(max_apn, 'a', 63u);
    max_apn[63] = '.';
    memset(&max_apn[64], 'b', MODEM_DIAG_PACKET_APN_MAX - 64u);
    max_apn[MODEM_DIAG_PACKET_APN_MAX] = '\0';
    char packet_line[384];
    (void)snprintf(packet_line, sizeof(packet_line),
                   "+CGCONTRDP: 1,5,\"%s\"", max_apn);
    reset_diag_snapshot(&diag);
    check(telit_diag_parse_cgcontrdp(packet_line, &diag) ==
                  MODEM_DIAG_LINE_ACCEPT &&
              strncmp(diag.packet.apn, max_apn,
                      MODEM_DIAG_PACKET_APN_PREVIEW_MAX) == 0 &&
              strlen(diag.packet.apn) ==
                  MODEM_DIAG_PACKET_APN_PREVIEW_MAX &&
              (diag.group[MODEM_DIAG_GROUP_PACKET].present_fields &
               MODEM_DIAG_PACKET_APN_TRUNCATED) != 0u,
          "CGCONTRDP accepts a maximum APN and marks its bounded preview");

    char ipv6_address_mask[MODEM_DIAG_PACKET_ADDRESS_MAX + 1u];
    size_t ipv6_length = 0u;
    for (size_t i = 0u; i < 32u; i++) {
        if (i != 0u) {
            ipv6_address_mask[ipv6_length++] = '.';
        }
        memcpy(&ipv6_address_mask[ipv6_length], "255", 3u);
        ipv6_length += 3u;
    }
    ipv6_address_mask[ipv6_length] = '\0';
    check(ipv6_length == MODEM_DIAG_PACKET_ADDRESS_MAX,
          "IPv6 address-and-mask fixture reaches Telit's documented maximum");
    (void)snprintf(packet_line, sizeof(packet_line),
                   "+CGCONTRDP: 1,5,apn.example,\"%s\"",
                   ipv6_address_mask);
    reset_diag_snapshot(&diag);
    check(telit_diag_parse_cgcontrdp(packet_line, &diag) ==
                  MODEM_DIAG_LINE_ACCEPT &&
              strncmp(diag.packet.address, ipv6_address_mask,
                      MODEM_DIAG_PACKET_ADDRESS_PREVIEW_MAX) == 0 &&
              strlen(diag.packet.address) ==
                  MODEM_DIAG_PACKET_ADDRESS_PREVIEW_MAX &&
              (diag.group[MODEM_DIAG_GROUP_PACKET].present_fields &
               MODEM_DIAG_PACKET_ADDRESS_TRUNCATED) != 0u,
          "CGCONTRDP accepts full IPv6 and marks its bounded preview");

    modem_diag_packet_t packet_before = diag.packet;
    uint64_t packet_fields_before =
        diag.group[MODEM_DIAG_GROUP_PACKET].present_fields;
    check(telit_diag_parse_cgcontrdp(
              "+CGCONTRDP: 1,5,valid.apn,\"1.2.3.4\",bad\"tail",
              &diag) == MODEM_DIAG_LINE_INVALID &&
              memcmp(&diag.packet, &packet_before, sizeof(packet_before)) == 0 &&
              diag.group[MODEM_DIAG_GROUP_PACKET].present_fields ==
                  packet_fields_before,
          "a malformed optional tail cannot partially mutate packet state");

    char too_long_apn[MODEM_DIAG_PACKET_APN_MAX + 2u];
    memcpy(too_long_apn, max_apn, MODEM_DIAG_PACKET_APN_MAX);
    too_long_apn[MODEM_DIAG_PACKET_APN_MAX] = 'c';
    too_long_apn[MODEM_DIAG_PACKET_APN_MAX + 1u] = '\0';
    (void)snprintf(packet_line, sizeof(packet_line),
                   "+CGCONTRDP: 1,5,\"%s\"", too_long_apn);
    check(telit_diag_parse_cgcontrdp(packet_line, &diag) ==
                  MODEM_DIAG_LINE_INVALID,
          "CGCONTRDP rejects an APN beyond the standards wire domain");

    char too_long_address[MODEM_DIAG_PACKET_ADDRESS_MAX + 2u];
    memset(too_long_address, '1', MODEM_DIAG_PACKET_ADDRESS_MAX + 1u);
    too_long_address[MODEM_DIAG_PACKET_ADDRESS_MAX + 1u] = '\0';
    (void)snprintf(packet_line, sizeof(packet_line),
                   "+CGCONTRDP: 1,5,valid.apn,\"%s\"",
                   too_long_address);
    check(telit_diag_parse_cgcontrdp(packet_line, &diag) ==
                  MODEM_DIAG_LINE_INVALID &&
              memcmp(&diag.packet, &packet_before, sizeof(packet_before)) == 0 &&
              diag.group[MODEM_DIAG_GROUP_PACKET].present_fields ==
                  packet_fields_before,
          "over-domain packet fields are rejected without partial mutation");
    check(telit_diag_parse_cgcontrdp(
              "+CGCONTRDP: 1,5,\"bad,apn\"", &diag) ==
                  MODEM_DIAG_LINE_INVALID &&
              telit_diag_parse_cgcontrdp(
                  "+CGCONTRDP: 1,5,\"unterminated", &diag) ==
                  MODEM_DIAG_LINE_INVALID &&
              telit_diag_parse_cgcontrdp(
                  "+CGCONTRDP: 1,5,-bad.apn", &diag) ==
                  MODEM_DIAG_LINE_INVALID,
          "CGCONTRDP rejects malformed APN labels, quoting, and separators");

    reset_diag_snapshot(&diag);
    check(telit_diag_parse_stune_enabled("#STUNEANT: 1", &diag) ==
              MODEM_DIAG_LINE_ACCEPT &&
              telit_diag_parse_stune_capability(
                  "#STUNEANT: (0,1),(7FD9FFFF),(0,1),(0,1)", &diag) ==
                  MODEM_DIAG_LINE_ACCEPT &&
              telit_diag_parse_gtune("#GTUNEANT: 601840A7,0,0", &diag) ==
                  MODEM_DIAG_LINE_ACCEPT &&
              telit_diag_parse_gtune("#GTUNEANT: 10300,0,1", &diag) ==
                  MODEM_DIAG_LINE_ACCEPT &&
              telit_diag_parse_gtune("#GTUNEANT: 1F80BC50,1,0", &diag) ==
                  MODEM_DIAG_LINE_ACCEPT &&
              telit_diag_parse_gtune("#GTUNEANT: 400008,1,1", &diag) ==
                  MODEM_DIAG_LINE_ACCEPT &&
              telit_diag_group_finish(MODEM_DIAG_GROUP_TUNER, &diag),
          "bench-qualified WWX tuner table parses as one complete group");
    check(diag.tuner.table_complete && diag.tuner.table_exact &&
              diag.tuner.row_count == 4u,
          "tuner validator distinguishes complete exact policy");

    reset_diag_snapshot(&diag);
    check(telit_diag_parse_csms("+CSMS: 1,1,1,1", &diag) ==
              MODEM_DIAG_LINE_ACCEPT &&
              telit_diag_parse_cnmi("+CNMI: 2,1,0,0,0", &diag) ==
                  MODEM_DIAG_LINE_ACCEPT &&
              telit_diag_parse_csmp("+CSMP: 17,167,0,0", &diag) ==
                  MODEM_DIAG_LINE_ACCEPT &&
              telit_diag_parse_csca("+CSCA: \"+13123149810\",145", &diag) ==
                  MODEM_DIAG_LINE_ACCEPT &&
              telit_diag_parse_csdh("+CSDH: 0", &diag) ==
                  MODEM_DIAG_LINE_ACCEPT &&
              telit_diag_parse_ismscfg_v2("#ISMSCFG: 1", &diag) ==
                  MODEM_DIAG_LINE_ACCEPT &&
              telit_diag_parse_mwi_v2("#MWI: 1,1,1,2", &diag) ==
                  MODEM_DIAG_LINE_ACCEPT &&
              telit_diag_parse_mwi_v2("#MWI: 1,1,3,4", &diag) ==
                  MODEM_DIAG_LINE_ACCEPT &&
              telit_diag_parse_mwi_v2("#MWI: 1,1,4,5", &diag) ==
                  MODEM_DIAG_LINE_ACCEPT &&
              telit_diag_group_finish(MODEM_DIAG_GROUP_SMS_CONFIG, &diag),
          "SMS configuration commits only after all mandatory rows parse");
    check_string(diag.sms.service_center, "+13123149810",
                 "typed SMSC number");
    check(diag.sms.ims_mode == 1u &&
              diag.sms.message_waiting
                  .category[MODEM_MESSAGE_WAITING_VOICE_LINE_1].active &&
              diag.sms.message_waiting
                      .category[MODEM_MESSAGE_WAITING_VOICE_LINE_1].count ==
                  2u &&
              diag.sms.message_waiting
                  .category[MODEM_MESSAGE_WAITING_FAX].active &&
              diag.sms.message_waiting
                      .category[MODEM_MESSAGE_WAITING_FAX].count == 4u &&
              diag.sms.message_waiting
                  .category[MODEM_MESSAGE_WAITING_EMAIL].active &&
              diag.sms.message_waiting
                      .category[MODEM_MESSAGE_WAITING_EMAIL].count == 5u,
          "typed Telit SMS/IMS and multi-category MWI fields");

    reset_diag_snapshot(&diag);
    check(telit_diag_parse_cpms_v2(
              "+CPMS: \"ME\",3,255,\"ME\",3,255,\"ME\",3,255", &diag) ==
              MODEM_DIAG_LINE_ACCEPT &&
              telit_diag_parse_cpbs_v2("+CPBS: \"ME\",7,500", &diag) ==
                  MODEM_DIAG_LINE_ACCEPT &&
              telit_diag_group_finish(MODEM_DIAG_GROUP_STORAGE_CAPS, &diag),
          "SMS and phonebook storage capabilities form one coherent group");
    check(diag.storage.sms_used == 3u && diag.storage.sms_total == 255u &&
              diag.storage.phonebook_used == 7u &&
              diag.storage.phonebook_total == 500u,
          "typed storage counts match board transcript shapes");

    reset_diag_snapshot(&diag);
    check(telit_diag_parse_dvi("#DVI: 1,2,1", &diag) ==
              MODEM_DIAG_LINE_ACCEPT &&
              telit_diag_parse_dviext("#DVIEXT: 1,1,0,0,0", &diag) ==
                  MODEM_DIAG_LINE_ACCEPT &&
              telit_diag_group_finish(MODEM_DIAG_GROUP_DVI, &diag),
          "bench-observed DVI/DVIEXT readback validates");
    check(diag.dvi.enabled == 1u && diag.dvi.mode == 2u &&
              diag.dvi.clock == 1u && diag.dvi.config == 1u &&
              diag.dvi.sample_rate == 1u,
          "typed DVI values retain the production I2S configuration");

    reset_diag_snapshot(&diag);
    check(telit_diag_parse_model("LE910C1-WWX", &diag) ==
              MODEM_DIAG_LINE_ACCEPT &&
              telit_diag_parse_firmware("M0F.660006", &diag) ==
                  MODEM_DIAG_LINE_ACCEPT &&
              telit_diag_parse_imei("359000000000001", &diag) ==
                  MODEM_DIAG_LINE_ACCEPT &&
              telit_diag_group_finish(MODEM_DIAG_GROUP_IDENTITY, &diag),
          "plain-line identity responses are strict and complete");
    check(telit_diag_parse_firmware("RING", &diag) == MODEM_DIAG_LINE_IGNORE &&
              telit_diag_parse_imei("35900000000000X", &diag) ==
                  MODEM_DIAG_LINE_IGNORE,
          "plain-line identity parsers cannot swallow common URCs or bad IMEIs");
}

static void test_guarded_maintenance_boundary(void) {
    const modem_maintenance_backend_t *maintenance =
        &g_modem_vendor.maintenance;
    char command[96];
    uint16_t seconds = 0u;
    uint8_t mode = 0u;
    modem_band_config_t bands;
    modem_gpio_config_t gpio;
    bool enabled = false;

    check(maintenance->supported,
          "Telit exports guarded maintenance capability");
    check(maintenance->build_scan_timer_set(300u, command,
                                             sizeof(command)) &&
              strcmp(command, "AT#NWSCANTMR=300") == 0 &&
              !maintenance->build_scan_timer_set(4u, command,
                                                  sizeof(command)),
          "scan timer builder enforces the documented range");
    check(maintenance->parse_scan_timer("#NWSCANTMR: 300", &seconds) ==
              MODEM_DIAG_LINE_ACCEPT && seconds == 300u &&
              maintenance->parse_scan_timer("#NWSCANTMR: x", &seconds) ==
                  MODEM_DIAG_LINE_INVALID,
          "scan timer readback is strict and typed");
    check(maintenance->parse_scan_timer("+OTHER: 300", NULL) ==
              MODEM_DIAG_LINE_IGNORE &&
              maintenance->parse_scan_timer("#NWSCANTMR: 300", NULL) ==
                  MODEM_DIAG_LINE_INVALID &&
              maintenance->parse_band_mode("#SELBNDMODE: 1", NULL) ==
                  MODEM_DIAG_LINE_INVALID &&
              maintenance->parse_tuner_enabled("#STUNEANT: 1", NULL) ==
                  MODEM_DIAG_LINE_INVALID,
          "scalar maintenance parsers preserve ignore and null-output semantics");

    check(maintenance->build_band_mode_set(1u, command,
                                            sizeof(command)) &&
              strcmp(command, "AT#SELBNDMODE=1") == 0 &&
              maintenance->parse_band_mode("#SELBNDMODE: 1", &mode) ==
                  MODEM_DIAG_LINE_ACCEPT && mode == 1u,
          "RAM-band mode command and readback stay vendor-owned");
    check(maintenance->parse_band_nvm("#BND: 3,511,7FD9FFFF", &bands) ==
              MODEM_DIAG_LINE_ACCEPT && bands.gsm == 3u &&
              bands.wcdma == 511u && bands.lte == UINT64_C(0x7FD9FFFF),
          "persistent band tuple parses without string leakage");
    modem_band_config_t desired;
    check(maintenance->band_preset(2u, &bands, &desired) &&
              desired.gsm == bands.gsm &&
              desired.wcdma == bands.wcdma &&
              desired.lte == UINT64_C(0x8) &&
              maintenance->build_band_ram_set(&desired, command,
                                               sizeof(command)) &&
              strcmp(command, "AT#BNDRAM=3,511,8") == 0,
          "B4 preset changes only the LTE mask and builds a RAM-only write");
    bands.lte = UINT64_C(0x2);
    check(!maintenance->band_preset(2u, &bands, &desired),
          "preset cannot enable a band absent from the active baseline");

    check(maintenance->build_function_set(4u, command, sizeof(command)) &&
              strcmp(command, "AT+CFUN=4") == 0 &&
              maintenance->parse_function("+CFUN: 4", &mode) ==
                  MODEM_DIAG_LINE_ACCEPT && mode == 4u &&
              !maintenance->build_function_set(1u, command,
                                                sizeof(command)),
          "maintenance CFUN boundary exposes only offline and power-save modes");
    check(maintenance->build_tuner_enabled_set(false, command,
                                                sizeof(command)) &&
              strcmp(command, "AT#STUNEANT=0") == 0 &&
              maintenance->parse_tuner_enabled("#STUNEANT: 0", &enabled) ==
                  MODEM_DIAG_LINE_ACCEPT && !enabled,
          "tuner ownership command has exact typed readback");
    check(maintenance->build_gpio_set(2u, true, 1u, false, command,
                                      sizeof(command)) &&
              strcmp(command, "AT#GPIO=2,1,1,0") == 0 &&
              maintenance->parse_gpio("#GPIO: 1,1", &gpio) ==
                  MODEM_DIAG_LINE_ACCEPT && gpio.direction == 1u &&
              gpio.state,
          "GPIO maintenance preserves explicit runtime-only save policy");
}

static bool build_and_decode(const sms_deliver_t *d, sms_codec_message_t *decoded) {
    char hex[SMS_DELIVER_HEX_MAX];
    uint8_t tpdu = 0u;
    return sms_deliver_build(d, hex, sizeof(hex), &tpdu) && sms_pdu_decode(hex, decoded);
}

static void test_direct_3gpp2_text_forms(void) {
    sms_deliver_t d;
    sms_codec_message_t m;
    check(telit_translate_direct_sms(
              "+CMT: \"7866910488\",\"\",\"20260916105524\",129,4098,0,8,9",
              BYTES("Dhdjdjdjs"), &d) == MODEM_SMS_DIRECT_ACCEPTED,
          "3GPP2 text, Latin-1 body accepted");
    check(build_and_decode(&d, &m) && strcmp(m.address, "7866910488") == 0 &&
              strcmp(m.text, "Dhdjdjdjs") == 0 &&
              strcmp(m.timestamp, "26/09/16,10:55:24") == 0,
          "Latin-1 body re-encoded as GSM-7 with the delivery time");

    check(telit_translate_direct_sms(
              "+CMT: \"7866910488\",\"\",\"20260916105553\",129,4101,0,9,23",
              BYTES("050003620202D46435599D9EABE7EAB99AAC26ABC9"), &d) == MODEM_SMS_DIRECT_ACCEPTED,
          "WEMT GSM-7 hex body accepted");
    check(d.udhi && d.udl == 24u && d.ud_len == 21u,
          "UDH kept and non-padding final septet retained despite length 23");
    check(build_and_decode(&d, &m) && m.has_concat && m.concat_ref == 0x62u &&
              m.concat_total == 2u && m.concat_seq == 2u,
          "concatenation survives");

    check(telit_translate_direct_sms(
              "+CMT: \"12025550123\",\"\",\"20260918102440\",129,4101,1,0,15",
              BYTES("0B0504158A158A000302030242494E"), &d) == MODEM_SMS_DIRECT_ACCEPTED &&
              d.udhi && build_and_decode(&d, &m) && m.binary &&
              m.has_ports && m.dest_port == 0x158au && m.source_port == 0x158au &&
              m.has_concat && m.concat_ref == 2u && m.concat_total == 3u &&
              m.concat_seq == 2u && m.binary_len == 3u &&
              memcmp(m.binary_data, "BIN", 3u) == 0,
          "WEMT octet picture retains ports, concatenation and payload");
    check(telit_translate_direct_sms(
              "+CMT: \"12025550123\",\"\",\"20260918102440\",129,4098,1,0,3",
              BYTES("42494E"), &d) == MODEM_SMS_DIRECT_ACCEPTED && !d.udhi,
          "ordinary octet data does not acquire a speculative header");

    check(telit_translate_direct_sms(
              "+CMT: \"7866910488\",\"\",\"20260916105559\",129,4098,0,4,2",
              BYTES("D83EDD2A"), &d) == MODEM_SMS_DIRECT_ACCEPTED &&
              d.dcs == 0x08u && d.udl == 4u && d.ud_len == 4u,
          "Unicode body -> UCS2 DCS");
    check(d.ud[0] == 0xD8u && d.ud[1] == 0x3Eu && d.ud[2] == 0xDDu && d.ud[3] == 0x2Au,
          "UTF-16BE code units copied verbatim");
    /* sms_pdu_decode renders surrogate pairs as '?' by design. */
    check(build_and_decode(&d, &m) && strcmp(m.text, "??") == 0,
          "emoji surrogate pair decodes as the '?' fallback");

    check(telit_translate_direct_sms(
              "+CMT: \"7866910488\",\"\",\"20260916105559\",129,4099,0,8,1",
              BYTES("3"), &d) == MODEM_SMS_DIRECT_REJECTED,
          "voice mail notification teleservice rejected");
    check(telit_translate_direct_sms(
              "+CMT: \"+18132936877\",,\"26/09/16,10:59:04-16\",145,4,0,0,\"+19037029920\",145,7",
              BYTES("Djssjjs"), &d) == MODEM_SMS_DIRECT_ACCEPTED,
          "3GPP plain text applies Telit's character-count convention");
    check(telit_translate_direct_sms("+CMT: ,26", BYTES("07"), &d) == MODEM_SMS_DIRECT_NOT_MINE,
          "3GPP PDU form is not claimed");

    /* Zero-length bodies: the collector completes them on the header alone. */
    check(telit_translate_direct_sms(
              "+CMT: \"7866910488\",\"\",\"20260916105524\",129,4098,0,8,0",
              BYTES(""), &d) == MODEM_SMS_DIRECT_ACCEPTED &&
              d.ud_len == 0u && d.udl == 0u && d.dcs == 0x00u,
          "3GPP2 text enc 8 with length 0 -> empty GSM-7 UD");
    check(build_and_decode(&d, &m) && m.text[0] == '\0' &&
              strcmp(m.address, "7866910488") == 0,
          "empty Latin-1 body builds and decodes");
    check(telit_translate_direct_sms(
              "+CMT: \"7866910488\",\"\",\"20260916105524\",129,4098,0,9,0",
              BYTES(""), &d) == MODEM_SMS_DIRECT_ACCEPTED &&
              d.ud_len == 0u && d.udl == 0u && d.dcs == 0x00u && !d.udhi,
          "3GPP2 text enc 9 with length 0 -> empty GSM-7 UD");
    check(telit_translate_direct_sms(
              "+CMT: \"7866910488\",\"\",\"20260916105524\",129,4098,0,4,0",
              BYTES(""), &d) == MODEM_SMS_DIRECT_ACCEPTED &&
              d.ud_len == 0u && d.udl == 0u && d.dcs == 0x08u,
          "3GPP2 text enc 4 with length 0 -> empty UCS2 UD");
    check(telit_translate_direct_sms(
              "+CMT: \"7866910488\",\"\",\"20260916105524\",129,4098,0,0,0",
              BYTES(""), &d) == MODEM_SMS_DIRECT_ACCEPTED &&
              d.ud_len == 0u && d.udl == 0u && d.dcs == 0x04u,
          "3GPP2 text enc 0 with length 0 -> empty octet UD");
    check(telit_translate_direct_sms(
              "+CMT: \"7866910488\",\"\",\"20260916105524\",129,4098,0,9,0",
              BYTES("41"), &d) == MODEM_SMS_DIRECT_REJECTED,
          "a body that contradicts length 0 is rejected");

    /* Plain bodies are validated against <length> (the RAW reader delivers
     * exactly that many bytes; CR, LF, 0x00 and ESC+x count 1:1). */
    check(telit_translate_direct_sms(
              "+CMT: \"7866910488\",\"\",\"20260916105524\",129,4098,0,8,9",
              BYTES("Dhdjdjdj"), &d) == MODEM_SMS_DIRECT_REJECTED,
          "enc 8 body shorter than <length> rejected");
    check(telit_translate_direct_sms(
              "+CMT: \"7866910488\",\"\",\"20260916105524\",129,4098,0,8,9",
              BYTES("Dhdjdjdjsx"), &d) == MODEM_SMS_DIRECT_REJECTED,
          "enc 8 body longer than <length> rejected");
    check(telit_translate_direct_sms(
              "+CMT: \"7866910488\",\"\",\"20260916105524\",129,4098,0,8,9",
              BYTES("Hi\r\nthere"), &d) == MODEM_SMS_DIRECT_ACCEPTED &&
              build_and_decode(&d, &m) && strcmp(m.text, "Hi\r\nthere") == 0,
          "enc 8 body with an embedded CRLF round-trips");
    static const uint8_t at_body[3] = {'a', 0x00u, 'b'};
    check(telit_translate_direct_sms(
              "+CMT: \"7866910488\",\"\",\"20260916105524\",129,4098,0,8,3",
              at_body, sizeof(at_body), &d) == MODEM_SMS_DIRECT_ACCEPTED &&
              build_and_decode(&d, &m) && strcmp(m.text, "a@b") == 0,
          "enc 8 body with GSM 0x00 (@) round-trips");
    static const uint8_t high_body[2] = {'a', 0xE9u};
    check(telit_translate_direct_sms(
              "+CMT: \"7866910488\",\"\",\"20260916105524\",129,4098,0,8,2",
              high_body, sizeof(high_body), &d) == MODEM_SMS_DIRECT_REJECTED,
          "enc 8 text-form byte >= 0x80 rejected");
    check(telit_translate_direct_sms(
              "+CMT: \"7866910488\",\"\",\"20260916105524\",129,4098,0,9,7",
              BYTES("Djssjj\n"), &d) == MODEM_SMS_DIRECT_ACCEPTED &&
              build_and_decode(&d, &m) && strcmp(m.text, "Djssjj\n") == 0,
          "enc 9 plain body with a trailing LF round-trips");
}

static void test_direct_3gpp_character_length(void) {
    static const char header[] =
        "+CMT: \"15551230000\",,\"26/09/19,11:17:50-16\",129,4,0,0,,129,80";
    uint8_t body[158u];
    for (size_t i = 0u; i < sizeof(body); i += 2u) {
        body[i] = 0x1bu;
        body[i + 1u] = 0x28u;
    }
    body[136u] = '\r';
    body[137u] = '\n';
    char hex[SMS_DELIVER_HEX_MAX];
    uint8_t tpdu = 0u;
    modem_sms_direct_reset();
    bool ok = modem_sms_direct_feed(header, telit_translate_direct_sms,
        hex, sizeof(hex), &tpdu) == MODEM_SMS_DIRECT_STEP_HEADER;
    for (size_t i = 0u; i < sizeof(body); i++) {
        ok = modem_sms_direct_feed_raw(body[i], telit_translate_direct_sms,
            hex, sizeof(hex), &tpdu) == MODEM_SMS_DIRECT_STEP_IGNORED && ok;
    }
    ok = modem_sms_direct_feed_raw('\r', telit_translate_direct_sms,
        hex, sizeof(hex), &tpdu) == MODEM_SMS_DIRECT_STEP_IGNORED && ok;
    ok = modem_sms_direct_feed_raw('\n', telit_translate_direct_sms,
        hex, sizeof(hex), &tpdu) == MODEM_SMS_DIRECT_STEP_READY && ok;
    sms_codec_message_t decoded;
    check(ok && sms_pdu_decode(hex, &decoded) && strlen(decoded.text) == 80u &&
              decoded.text[68] == '\r' && decoded.text[69] == '\n',
          "escaped GSM characters and embedded CRLF survive beyond packed-ASCII bound");
    sms_deliver_t d;
    check(telit_translate_direct_sms(header, body, sizeof(body) - 1u, &d) ==
              MODEM_SMS_DIRECT_REJECTED,
          "dangling GSM escape is rejected");
    body[0] = 0x80u;
    check(telit_translate_direct_sms(header, body, sizeof(body), &d) ==
              MODEM_SMS_DIRECT_REJECTED,
          "non-GSM bytes remain rejected");
}

static void test_direct_3gpp2_wemt_length_boundary(void) {
    static const char first[] =
        "050003170201A6E9791DD4AEB3E96978584E078DD1E5F1DA059A96CFEDB29B0E12BF"
        "EB6E72589E2ECF41EDFA9C0E82CBCBF3B2DC5E0695ED65791E344687E5E131BD2C07"
        "85DD64101D5D06B9CB783AA85D9ECFC3E732A85D9FD341737A9ACD0685E5F2B4BDEC"
        "0251D1E939685E76D3CBEE7119442EB3D3E2B23C4C2FB3F3207A785D9E83E8E83268"
        "DA9C82C4";
    static const char second[] =
        "050003170202CAF9B79B0C7ABBCBA079F9DC2EBBE92E50D14D06B5C3F27559AE030D"
        "9F4D28B3482DBA1A";
    static const char expected[] =
        "Sisu multipart check. Segment boundaries must preserve every character "
        "and the next message must still arrive. This sentence deliberately takes "
        "the SMS beyond one segment. End marker: COMPLETE.";
    sms_deliver_t d;
    sms_codec_message_t m;
    char joined[sizeof(expected)];
    joined[0] = '\0';
    check(telit_translate_direct_sms(
              "+CMT: \"12025550123\",\"\",\"20260916201951\",129,4101,0,9,159",
              BYTES(first), &d) == MODEM_SMS_DIRECT_ACCEPTED &&
              d.udl == 160u && d.ud_len == 140u,
          "WEMT 159/160 ambiguity retains the non-padding final septet");
    if (build_and_decode(&d, &m)) {
        snprintf(joined, sizeof(joined), "%s", m.text);
        check(strlen(m.text) == 153u && m.text[152] == 'b',
              "first full WEMT part retains the boundary character");
    } else {
        check(false, "first WEMT boundary fixture decodes");
    }
    check(telit_translate_direct_sms(
              "+CMT: \"12025550123\",\"\",\"20260916201957\",129,4101,0,9,47",
              BYTES(second), &d) == MODEM_SMS_DIRECT_ACCEPTED && d.udl == 47u,
          "WEMT CR padding is not appended as another text character");
    if (build_and_decode(&d, &m)) {
        size_t used = strlen(joined);
        snprintf(joined + used, sizeof(joined) - used, "%s", m.text);
        check_string(joined, expected, "live multipart fixture round-trips exactly");
    } else {
        check(false, "second WEMT boundary fixture decodes");
    }
    check(telit_translate_direct_sms(
              "+CMT: \"12025550123\",\"\",\"20260916201951\",129,4101,0,9,158",
              BYTES(first), &d) == MODEM_SMS_DIRECT_REJECTED,
          "padding evidence does not widen the one-septet correction bound");

    /* At each ambiguous octet boundary, preserve zero/CR padding, but retain
     * every other septet value as an actual additional character. */
    for (unsigned udl = 15u; udl < 160u; udl += 8u) {
        uint8_t bytes[140] = {5u, 0u, 3u, 1u, 2u, 1u};
        size_t octets = (udl * 7u + 7u) / 8u;
        char header[100];
        snprintf(header, sizeof(header),
                 "+CMT: \"12025550123\",\"\",\"20260916201951\",129,4101,0,9,%u",
                 udl);
        for (unsigned tail = 0u; tail < 128u; tail++) {
            bytes[octets - 1u] = (uint8_t)(tail << 1);
            char hex[281];
            for (size_t i = 0u; i < octets; i++) {
                snprintf(hex + i * 2u, sizeof(hex) - i * 2u, "%02X", bytes[i]);
            }
            unsigned expected_udl = udl + (tail != 0u && tail != 13u ? 1u : 0u);
            if (telit_translate_direct_sms(header, BYTES(hex), &d) !=
                    MODEM_SMS_DIRECT_ACCEPTED || d.udl != expected_udl ||
                d.ud_len != octets || memcmp(d.ud, bytes, octets) != 0) {
                check(false, "WEMT ambiguous lengths preserve data and padding");
                return;
            }
        }
    }
}

static void test_direct_3gpp2_pdu_forms(void) {
    sms_deliver_t d;
    sms_codec_message_t m;
    check(telit_translate_direct_sms("+CMT: \"7866910488\",\"\",25",
              BYTES("068187661940882609161059281002000807446A73736A6A73"), &d) ==
              MODEM_SMS_DIRECT_ACCEPTED,
          "3GPP2 PDU form accepted");
    check(build_and_decode(&d, &m) && strcmp(m.address, "7866910488") == 0 &&
              strcmp(m.text, "Djssjjs") == 0 &&
              strcmp(m.timestamp, "26/09/16,10:59:28") == 0,
          "PDU-form fields map to a DELIVER");
    check(telit_translate_direct_sms("+CMT: \"7866910488\",\"\",22",
              BYTES("068187661940882609161127491002000404D83DDE1C"), &d) ==
              MODEM_SMS_DIRECT_ACCEPTED && d.dcs == 0x08u && d.ud_len == 4u,
          "PDU-form Unicode");
    check(d.ud[0] == 0xD8u && d.ud[1] == 0x3Du && d.ud[2] == 0xDEu && d.ud[3] == 0x1Cu &&
              build_and_decode(&d, &m) && strcmp(m.text, "??") == 0,
          "PDU-form emoji code units copied, decoder '?' fallback");
    check(telit_translate_direct_sms("+CMT: \"7866910488\",\"\",32",
              BYTES("06818766194088260916112747100500090F6A721A4D4693D56435594D469301"), &d) ==
              MODEM_SMS_DIRECT_ACCEPTED && !d.udhi && d.udl == 15u && d.ud_len == 14u,
          "PDU-form GSM-7 (UDH stripped by the module) accepted as a plain part");
    check(build_and_decode(&d, &m) && strcmp(m.text, "jdihdhdjdjdjdhd") == 0,
          "PDU-form GSM-7 decodes");

    /* Bench 2026-09-16: the module's text-form <length> is off by one on
     * some WEMT parts (21 given, 20 hex octets = 22 septets: 7 UDH+fill + 15
     * text). The octet count is authoritative; UDL is taken from {L, L+1, L-1}. */
    check(telit_translate_direct_sms(
              "+CMT: \"7866910488\",\"\",\"20260916150755\",129,4101,0,9,21",
              BYTES("050003FB0202D4E4349A8C26ABC96AB29A8C2603"), &d) ==
              MODEM_SMS_DIRECT_ACCEPTED && d.udhi && d.udl == 22u && d.ud_len == 20u,
          "WEMT part with <length> off by one is accepted with UDL from the octets");
    check(build_and_decode(&d, &m) && strcmp(m.text, "jdihdhdjdjdjdhd") == 0 &&
              m.has_concat && m.concat_ref == 0xFBu && m.concat_total == 2u &&
              m.concat_seq == 2u,
          "the off-by-one WEMT part round-trips with its concat fields");
    check(telit_translate_direct_sms(
              "+CMT: \"7866910488\",\"\",\"20260916150755\",129,4101,0,9,21",
              BYTES("050003FB0202D4E4349A8C26ABC96AB29A8C260300"), &d) ==
              MODEM_SMS_DIRECT_REJECTED,
          "a GSM-7 hex body off by two octets is still rejected");
    check(telit_translate_direct_sms("+CMT: \"7866910488\",\"\",25",
              BYTES("068187661940882609161059281002000807446A73736A6A"), &d) ==
              MODEM_SMS_DIRECT_REJECTED,
          "short payload rejected");
}

/* IS-637 encodings 2 (7-bit ASCII) and 3 (IA5): the text form carries the
 * plain text with <length> = PACKED OCTET count (some modules report
 * characters); the PDU form carries <data_len> = CHARACTER count and the
 * data packed 7 bits per char MSB-first. Bench self-loopback 2026-09-15. */
static void test_direct_3gpp2_ascii7(void) {
    sms_deliver_t d;
    sms_codec_message_t m;
    static const char TEXT_HEADER[] =
        "+CMT: \"8132936877\",\"\",\"20260915193325\",129,4098,1,2,14";
    check(telit_translate_direct_sms(TEXT_HEADER, BYTES("Warp loopback 2"), &d) ==
              MODEM_SMS_DIRECT_ACCEPTED,
          "enc 2 text form with <length> = packed octets accepted");
    check(build_and_decode(&d, &m) && strcmp(m.text, "Warp loopback 2") == 0 &&
              strcmp(m.address, "8132936877") == 0 &&
              strcmp(m.timestamp, "26/09/15,19:33:25") == 0,
          "enc 2 text form round-trips text, sender and time");
    /* Only the packed relation holds: a character-count <length> would let a
     * body truncated at a line break pass as complete (owner audit P2). */
    check(telit_translate_direct_sms(
              "+CMT: \"8132936877\",\"\",\"20260915193325\",129,4098,1,2,15",
              BYTES("Warp loopback 2"), &d) == MODEM_SMS_DIRECT_INCOMPLETE,
          "a larger packed length requires more characters, not a character-count match");
    check(telit_translate_direct_sms(TEXT_HEADER, BYTES("AAAAAAAAAAAAAA"), &d) ==
              MODEM_SMS_DIRECT_INCOMPLETE,
          "enc 2 body of exactly <length> characters is incomplete (13 packed octets)");
    check(telit_translate_direct_sms(TEXT_HEADER, BYTES("AAAAAAAAAAAAAA\nB"), &d) ==
              MODEM_SMS_DIRECT_ACCEPTED && build_and_decode(&d, &m) &&
              strcmp(m.text, "AAAAAAAAAAAAAA\nB") == 0,
          "enc 2 16-char body with an embedded LF packs to <length> 14");
    check(telit_translate_direct_sms(
              "+CMT: \"8132936877\",\"\",\"20260915193325\",129,4098,1,2,13",
              BYTES("Warp loopback 2"), &d) == MODEM_SMS_DIRECT_REJECTED,
          "enc 2 text form with an impossible <length> rejected");
    check(telit_translate_direct_sms(
              "+CMT: \"8132936877\",\"\",\"20260915193325\",129,4098,1,3,14",
              BYTES("Warp loopback 2"), &d) == MODEM_SMS_DIRECT_ACCEPTED,
          "enc 3 (IA5) text form follows the same rule");

    check(telit_translate_direct_sms("+CMT: \"8132936877\",\"\",32",
              BYTES("06811823398677260915193259100201020FAF8797041B37EFE18B0E3D681900"),
              &d) == MODEM_SMS_DIRECT_ACCEPTED,
          "enc 2 PDU form with <data_len> = characters accepted");
    check(build_and_decode(&d, &m) && strcmp(m.text, "Warp loopback 2") == 0 &&
              strcmp(m.address, "8132936877") == 0 &&
              strcmp(m.timestamp, "26/09/15,19:32:59") == 0,
          "enc 2 PDU form unpacks MSB-first and round-trips");
    check(telit_translate_direct_sms("+CMT: \"8132936877\",\"\",31",
              BYTES("06811823398677260915193259100201020FAF8797041B37EFE18B0E3D6819"),
              &d) == MODEM_SMS_DIRECT_REJECTED,
          "enc 2 PDU form with 13 octets for 15 characters rejected");

    /* Unpack helper: 2 chars ("Hi") and the 8-chars-in-7-octets boundary. */
    uint8_t out[8];
    static const uint8_t two[2] = {0x91u, 0xA4u}; /* 1001000 1101001 00 */
    check(telit_unpack_ascii7(two, sizeof(two), 2u, out) && out[0] == 'H' && out[1] == 'i',
          "7-bit ASCII unpack: two characters");
    static const uint8_t eight[7] = {0x83u, 0x0Au, 0x1Cu, 0x48u, 0xB1u, 0xA3u, 0xC8u};
    check(telit_unpack_ascii7(eight, sizeof(eight), 8u, out) &&
              memcmp(out, "ABCDEFGH", 8u) == 0,
          "7-bit ASCII unpack: eight characters fill seven octets exactly");
    check(!telit_unpack_ascii7(eight, 6u, 8u, out) && !telit_unpack_ascii7(eight, 7u, 9u, out),
          "7-bit ASCII unpack rejects an octet/character mismatch");

    check(telit_translate_direct_sms(TEXT_HEADER, BYTES("AAAAAAAAAAAAAAAAB"), &d) ==
              MODEM_SMS_DIRECT_REJECTED,
          "an oversized ASCII body is a final rejection");
    static const uint8_t invalid_prefix[] = {'A', 0xE9u};
    check(telit_translate_direct_sms(TEXT_HEADER, invalid_prefix, sizeof(invalid_prefix), &d) ==
              MODEM_SMS_DIRECT_REJECTED,
          "an invalid short ASCII prefix is not allowed to consume another line");
}

static void test_direct_3gpp2_ascii7_line_breaks(void) {
    char header[128];
    uint8_t body[160];
    char hex[SMS_DELIVER_HEX_MAX];
    sms_codec_message_t decoded;
    unsigned cases = 0u;
    for (unsigned enc = 2u; enc <= 3u; enc++) {
        for (size_t n = 1u; n <= sizeof(body); n++) {
            snprintf(header, sizeof(header),
                     "+CMT: \"123\",\"\",\"20260915193325\",129,4098,0,%u,%u",
                     enc, (unsigned)((n * 7u + 7u) / 8u));
            for (size_t pos = 0u; pos < n; pos++) {
                for (unsigned kind = 0u; kind < 3u; kind++) {
                    if (kind == 2u && pos + 1u == n) {
                        continue;
                    }
                    memset(body, 'A', n);
                    body[pos] = kind == 0u ? '\n' : '\r';
                    if (kind == 2u) {
                        body[pos + 1u] = '\n';
                    }
                    uint8_t tpdu = 0u;
                    modem_sms_direct_reset();
                    bool ok = modem_sms_direct_feed(header, telit_translate_direct_sms,
                                                    hex, sizeof(hex), &tpdu) ==
                              MODEM_SMS_DIRECT_STEP_HEADER;
                    for (size_t i = 0u; i < n + 2u && ok; i++) {
                        uint8_t byte = i < n ? body[i] : i == n ? '\r' : '\n';
                        modem_sms_direct_step_t step = modem_sms_direct_feed_raw(
                            byte, telit_translate_direct_sms, hex, sizeof(hex), &tpdu);
                        ok = step == (i == n + 1u ? MODEM_SMS_DIRECT_STEP_READY
                                                 : MODEM_SMS_DIRECT_STEP_IGNORED);
                    }
                    ok = ok && !modem_sms_direct_pending() &&
                         sms_pdu_decode(hex, &decoded) && strlen(decoded.text) == n &&
                         memcmp(decoded.text, body, n) == 0;
                    if (!ok) {
                        fprintf(stderr, "ASCII framing: enc=%u length=%zu pos=%zu kind=%u\n",
                                enc, n, pos, kind);
                        check(false, "ASCII CR/LF positions preserve every body byte");
                        return;
                    }
                    cases++;
                }
            }
        }
    }
    check(cases == 76960u, "all ASCII/IA5 lengths and CR/LF positions round-trip");
}

static void test_direct_3gpp2_bounds(void) {
    sms_deliver_t d;
    sms_codec_message_t m;
    char header[128];
    char body[400];
    /* addr_len must cover the TOA byte and at most 10 BCD bytes. */
    check(telit_translate_direct_sms("+CMT: \"7866910488\",\"\",25",
              BYTES("FF8187661940882609161059281002000807446A73736A6A73"), &d) ==
              MODEM_SMS_DIRECT_REJECTED,
          "PDU-form oversized address length rejected");
    check(telit_translate_direct_sms("+CMT: \"7866910488\",\"\",25",
              BYTES("018187661940882609161059281002000807446A73736A6A73"), &d) ==
              MODEM_SMS_DIRECT_REJECTED,
          "PDU-form address without BCD bytes rejected");
    check(telit_translate_direct_sms("+CMT: \"7866910488\",\"\",25",
              BYTES("0681A7661940882609161059281002000807446A73736A6A73"), &d) ==
              MODEM_SMS_DIRECT_REJECTED,
          "PDU-form non-decimal BCD nibble rejected");
    check(telit_translate_direct_sms("+CMT: \"7866910488\",\"\",25",
              BYTES("0681876619408826A9161059281002000807446A73736A6A73"), &d) ==
              MODEM_SMS_DIRECT_REJECTED,
          "PDU-form non-decimal date nibble rejected");
    /* 20 digits (10 BCD bytes) is the DELIVER builder's address cap. */
    check(telit_translate_direct_sms("+CMT: \"12345678901234567890\",\"\",30",
              BYTES("0B81214365870921436587092609161059281002000807446A73736A6A73"), &d) ==
              MODEM_SMS_DIRECT_ACCEPTED && build_and_decode(&d, &m) &&
              strcmp(m.address, "12345678901234567890") == 0,
          "PDU-form 20-digit address accepted");
    /* Odd digit count uses the 0xF filler in the last high nibble. */
    check(telit_translate_direct_sms("+CMT: \"786691048\",\"\",25",
              BYTES("068187F61940882609161059281002000807446A73736A6A73"), &d) ==
              MODEM_SMS_DIRECT_REJECTED,
          "PDU-form filler nibble only valid in the last BCD byte");
    check(telit_translate_direct_sms("+CMT: \"786691048\",\"\",25",
              BYTES("068187661940F82609161059281002000807446A73736A6A73"), &d) ==
              MODEM_SMS_DIRECT_ACCEPTED && strcmp(d.address, "786691048") == 0,
          "PDU-form odd-length address accepted");

    memset(body, 'a', 160u);
    body[160] = '\0';
    snprintf(header, sizeof(header),
             "+CMT: \"7866910488\",\"\",\"20260916105524\",129,4098,0,8,%u", 160u);
    check(telit_translate_direct_sms(header, BYTES(body), &d) == MODEM_SMS_DIRECT_ACCEPTED &&
              d.udl == 160u && d.ud_len == 140u,
          "text-form 160-septet Latin-1 body fills the user data");
    body[160] = 'a';
    body[161] = '\0';
    check(telit_translate_direct_sms(header, BYTES(body), &d) == MODEM_SMS_DIRECT_REJECTED,
          "text-form 161-character body rejected");
    snprintf(header, sizeof(header),
             "+CMT: \"7866910488\",\"\",\"20260916105524\",129,4098,0,9,%u", 160u);
    body[160] = '\0';
    check(telit_translate_direct_sms(header, BYTES(body), &d) == MODEM_SMS_DIRECT_ACCEPTED &&
              d.dcs == 0x00u && !d.udhi && d.udl == 160u && d.ud_len == 140u,
          "text-form 160-septet plain GSM-7 body accepted");

    for (size_t i = 0u; i < 71u; i++) {
        memcpy(&body[i * 4u], "0041", 4u);
    }
    body[70u * 4u] = '\0';
    snprintf(header, sizeof(header),
             "+CMT: \"7866910488\",\"\",\"20260916105524\",129,4098,0,4,%u", 70u);
    check(telit_translate_direct_sms(header, BYTES(body), &d) == MODEM_SMS_DIRECT_ACCEPTED &&
              d.dcs == 0x08u && d.udl == 140u && d.ud_len == 140u,
          "text-form 70 code units of Unicode fill the user data");
    body[70u * 4u] = '0';
    body[71u * 4u] = '\0';
    snprintf(header, sizeof(header),
             "+CMT: \"7866910488\",\"\",\"20260916105524\",129,4098,0,4,%u", 71u);
    check(telit_translate_direct_sms(header, BYTES(body), &d) == MODEM_SMS_DIRECT_REJECTED,
          "text-form 71 code units of Unicode rejected");
    check(telit_translate_direct_sms(
              "+CMT: \"7866910488\",\"\",\"20260916105524\",129,4098,0,4,2",
              BYTES("D83EDD2A00"), &d) == MODEM_SMS_DIRECT_REJECTED,
          "text-form Unicode length/body mismatch rejected");
    check(telit_translate_direct_sms(
              "+CMT: \"7866910488\",\"\",\"20260916105524\",129,262144,0,8,1",
              BYTES("3"), &d) == MODEM_SMS_DIRECT_REJECTED,
          "alternate voice mail teleservice id rejected");
    check(telit_translate_direct_sms(
              "+CMT: \"7866910488\",\"\",\"20260916105524\",129,4098,0,5,1",
              BYTES("3"), &d) == MODEM_SMS_DIRECT_REJECTED,
          "unknown encoding rejected");
    check(telit_translate_direct_sms(
              "+CMT: \"7866910488\",\"\",\"20261316105524\",129,4098,0,8,1",
              BYTES("3"), &d) == MODEM_SMS_DIRECT_REJECTED,
          "impossible text-form date rejected");
    check(telit_translate_direct_sms("+CMT: \"7866910488\",\"\",25",
              BYTES("068187661940882609161059281003000807446A73736A6A73"), &d) ==
              MODEM_SMS_DIRECT_REJECTED,
          "PDU-form voice mail teleservice rejected");
    check(telit_translate_direct_sms("+CMT: \"7866910488\",\"\",1", BYTES("06"), &d) ==
              MODEM_SMS_DIRECT_REJECTED,
          "PDU-form truncated header rejected");

    /* PDU-form bodies at the 140-octet user-data maximum (the largest
     * payload the translator's scratch ever holds): 18 header bytes + 140. */
    static const char PDU_PREFIX_OCTET[] = "06818766194088260916105928100200008C";
    static const char PDU_PREFIX_UCS2[] = "06818766194088260916105928100200048C";
    memcpy(body, PDU_PREFIX_OCTET, sizeof(PDU_PREFIX_OCTET) - 1u);
    for (size_t i = 0u; i < 140u; i++) {
        memcpy(&body[sizeof(PDU_PREFIX_OCTET) - 1u + i * 2u], "5A", 2u);
    }
    body[sizeof(PDU_PREFIX_OCTET) - 1u + 280u] = '\0';
    snprintf(header, sizeof(header), "+CMT: \"7866910488\",\"\",%u", 158u);
    check(telit_translate_direct_sms(header, BYTES(body), &d) == MODEM_SMS_DIRECT_ACCEPTED &&
              d.dcs == 0x04u && d.udl == 140u && d.ud_len == 140u && d.ud[139] == 0x5Au,
          "PDU-form 140-octet 8-bit body fills the user data");
    memcpy(body, PDU_PREFIX_UCS2, sizeof(PDU_PREFIX_UCS2) - 1u);
    for (size_t i = 0u; i < 70u; i++) {
        memcpy(&body[sizeof(PDU_PREFIX_UCS2) - 1u + i * 4u], "0041", 4u);
    }
    body[sizeof(PDU_PREFIX_UCS2) - 1u + 280u] = '\0';
    check(telit_translate_direct_sms(header, BYTES(body), &d) == MODEM_SMS_DIRECT_ACCEPTED &&
              d.dcs == 0x08u && d.udl == 140u && d.ud_len == 140u && build_and_decode(&d, &m) &&
              strlen(m.text) == 70u && m.text[69] == 'A',
          "PDU-form 70-code-unit Unicode body fills the user data");
    memcpy(&body[sizeof(PDU_PREFIX_UCS2) - 1u + 280u], "00", 2u);
    body[sizeof(PDU_PREFIX_UCS2) - 1u + 282u] = '\0';
    snprintf(header, sizeof(header), "+CMT: \"7866910488\",\"\",%u", 159u);
    check(telit_translate_direct_sms(header, BYTES(body), &d) == MODEM_SMS_DIRECT_REJECTED,
          "PDU-form body past the user-data maximum rejected");
}

static void test_text_transmit_encoding(void) {
    modem_sms_text_t t;
    const uint8_t gsm[] = {'a', 0, 'b', ' ', 2, ' ', 0x11, ' ', 5};
    check(telit_encode_sms_text("a@b $ _ \xc3\xa9", &t) && t.dcs == 0u &&
          t.length == sizeof(gsm) && memcmp(t.body, gsm, sizeof(gsm)) == 0,
          "UTF-8 is mapped to GSM; embedded @ NUL uses an explicit byte length");
    check(telit_encode_sms_text("@{}\xc3\xb2\xe2\x82\xac\xd0\x9f", &t) && t.dcs == 8u &&
          t.length == 24u && memcmp(t.body, "0040007B007D00F220AC041F", 24u) == 0 && !t.multipart,
          "prompt editing controls and non-GSM text use UCS2 without a global charset change");
    char max[MODEM_SMS_TEXT_MAX + 2u];
    memset(max, 'A', MODEM_SMS_TEXT_MAX);
    max[MODEM_SMS_TEXT_MAX] = 0;
    check(telit_encode_sms_text(max, &t) && t.length == 160u && t.dcs == 0u && !t.multipart,
          "160 ordinary GSM characters retain one-segment encoding");
    memset(max, '{', MODEM_SMS_TEXT_MAX);
    check(telit_encode_sms_text(max, &t) && t.length == 640u && t.dcs == 8u && t.multipart,
          "maximum supported input fits UCS2 prompt and marks concatenation");
    max[MODEM_SMS_TEXT_MAX] = 'A'; max[MODEM_SMS_TEXT_MAX + 1u] = 0;
    check(!telit_encode_sms_text(max, &t), "oversized text is rejected, never truncated");
    const char *invalid[] = {"\xc0\x80", "\xe0\x80\x80", "\xed\xa0\x80", "\xe2\x82", "\xf0\x9f\x98\x80"};
    for (size_t i = 0u; i < sizeof(invalid) / sizeof(invalid[0]); i++)
        check(!telit_encode_sms_text(invalid[i], &t), "malformed UTF-8 and non-UCS2 characters fail explicitly");
    check(find_init_step("AT#CSCSEXT=0") != NULL && find_init_step("AT#CSCSEXT=1") == NULL,
          "startup explicitly disables the old global HEX experiment");
}

static void test_3gpp_ucs2_character_length(void) {
    sms_deliver_t d;
    const char *header = "+CMT: \"123\",,\"26/09/19,12:00:00+00\",129,4,0,8,,129,2";
    check(telit_translate_direct_sms(header, BYTES("0040007B"), &d) == MODEM_SMS_DIRECT_ACCEPTED &&
          d.udl == 4u && d.ud_len == 4u && d.ud[1] == '@' && d.ud[3] == '{',
          "UCS2 character count becomes exact octet count");
    check(telit_translate_direct_sms(header, BYTES("0040"), &d) == MODEM_SMS_DIRECT_REJECTED &&
          telit_translate_direct_sms(header, BYTES("0040007B0041"), &d) == MODEM_SMS_DIRECT_REJECTED &&
          telit_translate_direct_sms(header, BYTES("0040007Z"), &d) == MODEM_SMS_DIRECT_REJECTED,
          "short, oversized and non-hex UCS2 bodies do not consume following lines");
    const char *part = "06080400060202003300340035003600370038003900300031003200330034003500360037003800390030003100320033003400350036003700380039";
    check(telit_translate_direct_sms(
              "+CMT: \"+18132936877\",\"\",\"26/09/19,12:17:13-16\",145,68,0,8,\"+19039321415\",145,34",
              BYTES(part), &d) == MODEM_SMS_DIRECT_ACCEPTED && d.udhi && d.ud_len == 61u,
          "captured multipart UCS2 length includes seven UDH octets plus 27 characters");
    check(telit_translate_direct_sms(
              "+CMT: \"123\",,\"26/09/19,12:00:00+00\",129,68,0,8,,129,2",
              BYTES("FF000000"), &d) == MODEM_SMS_DIRECT_REJECTED,
          "UDH outside the UCS2 body is rejected");
}

static void test_sms_profile_readback(void) {
    const char *query = "AT+CMGF?;+CSDH?;+CSCS?;#CSCSEXT?;+CNMI?";
    const modem_provision_step_t *early = find_provision_step_nth(query, 0u);
    const modem_provision_step_t *late = find_provision_step_nth(query, 1u);
    check(early && late && early->recoverable && !late->recoverable,
          "early optional SMS inspection has a strict SIM-ready completion counterpart");
    if (!early || !late) return;
    const char *rows[] = {"+CNMI: 2,2,0,0,0", "#CSCSEXT: 0", "+CSCS: \"GSM\"", "+CMGF: 1", "+CSDH: 1"};
    char command[MODEM_PROVISION_COMMAND_MAX];
    check(early->build_set_cmd(command, sizeof(command)) &&
              strcmp(command, "AT+CMGF=1;+CSDH=1;+CSCS=\"GSM\";#CSCSEXT=0;+CNMI=2,2,0,0,0;&P0;&W0") == 0,
          "profile writer orders representation before delivery and explicitly saves default profile zero");
    early->readback_begin();
    for (unsigned i = 0; i < 5u; i++) early->parse_readback(rows[i]);
    check(early->readback_finish(true, false) == MODEM_PROVISION_LINE_MATCH,
          "profile verification requires all five settings and tolerates row order");
    early->readback_begin();
    for (unsigned i = 0; i < 4u; i++) early->parse_readback(rows[i]);
    check(early->readback_finish(true, false) == MODEM_PROVISION_LINE_INVALID,
          "an OK missing one SMS setting cannot qualify the profile");
    late->readback_begin();
    for (unsigned i = 0; i < 5u; i++) late->parse_readback(rows[i]);
    check(late->readback_finish(true, false) == MODEM_PROVISION_LINE_MISMATCH,
          "failed early inspection requires saving even after late runtime setters match");
    early->build_set_cmd(command, sizeof(command));
    early->readback_begin();
    for (unsigned i = 0; i < 5u; i++) early->parse_readback(rows[i]);
    early->parse_readback(rows[0]);
    check(early->readback_finish(true, false) == MODEM_PROVISION_LINE_INVALID,
          "duplicate profile fields cannot manufacture a verified aggregate");
    early->build_set_cmd(command, sizeof(command));
    early->readback_begin();
    for (unsigned i = 0; i < 5u; i++) early->parse_readback(i == 0u ? "+CNMI: 0,0,0,0,0" : rows[i]);
    check(early->readback_finish(true, false) == MODEM_PROVISION_LINE_MISMATCH,
          "stored-message delivery profile is a repairable mismatch");
}

int main(void) {
    test_sms_profile_readback();
    test_text_transmit_encoding();
    test_3gpp_ucs2_character_length();
    test_production_descriptor();
    test_init_table_direct_sms_delivery();
    test_antenna_tuner_policy();
    test_call_builders();
    test_call_forwarding_and_mailbox();
    test_ecam_mapping();
    test_sim_mwi_temperature_and_readbacks();
    test_parser_bounds();
    test_typed_diagnostics();
    test_guarded_maintenance_boundary();
    test_direct_3gpp2_text_forms();
    test_direct_3gpp_character_length();
    test_direct_3gpp2_wemt_length_boundary();
    test_direct_3gpp2_pdu_forms();
    test_direct_3gpp2_bounds();
    test_direct_3gpp2_ascii7();
    test_direct_3gpp2_ascii7_line_breaks();

    if (s_failures != 0) {
        fprintf(stderr, "%d Telit vendor test(s) failed\n", s_failures);
        return 1;
    }
    printf("Telit vendor protocol tests passed\n");
    return 0;
}
