#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "apps/net_monitor_logic.h"
#include "services/input_keys.h"

static int s_failures;

static void check(bool condition, const char *message) {
    if (!condition) {
        fprintf(stderr, "FAIL: %s\n", message);
        s_failures++;
    }
}

static uint32_t all_caps(void) {
    return NETMON_CAP_REVB2 | NETMON_CAP_TELIT | NETMON_CAP_MODEM |
           NETMON_CAP_TUNER | NETMON_CAP_VOICE |
           NETMON_CAP_LOCAL_CONTROLS | NETMON_CAP_RUNTIME_EXT |
           NETMON_CAP_RADIO_CONTROL | NETMON_CAP_RF_MAINTENANCE;
}

static uint32_t no_modem_caps(void) {
    return NETMON_CAP_REVB2 | NETMON_CAP_LOCAL_CONTROLS |
           NETMON_CAP_RUNTIME_EXT;
}

static void test_registry(void) {
    check(netmon_registry_validate(), "registry invariants");
    check(netmon_registry_count() > 60u, "full v2 page catalog present");
    check(netmon_registry_find(0u) == NULL, "page 00 is disable, not a page");
    check(netmon_registry_find(1u) != NULL, "serving page present");
    check(netmon_registry_find(99u) != NULL, "maintenance page present");
    check(netmon_registry_find(38u) == NULL &&
              netmon_registry_find(76u) == NULL,
          "retired duplicate pages stay absent");
    check(netmon_registry_find(82u) != NULL &&
              netmon_registry_find(89u) != NULL &&
              netmon_registry_find(89u)->action ==
                  NETMON_ACTION_BACKLIGHT_LEVEL,
          "boot timing moved to 82 and backlight control owns 89");

    uint8_t previous = 0u;
    for (size_t i = 0u; i < netmon_registry_count(); i++) {
        const netmon_page_descriptor_t *page = netmon_registry_at(i);
        check(page != NULL, "registry entry exists");
        if (page != NULL) {
            check(page->id > previous, "registry ids unique and ordered");
            previous = page->id;
        }
    }
}

static void test_capability_browse(void) {
    uint32_t local = NETMON_CAP_REVB2;
    check(netmon_adjacent_selector(0u, true, local) == 25u,
          "no-modem browse starts at first local page");
    check(netmon_adjacent_selector(25u, false, local) == 88u,
          "no-modem browse wraps over local pages only");
    check(netmon_adjacent_selector(0u, true, all_caps()) == 1u,
          "Telit browse starts at serving page");
    check(netmon_adjacent_selector(99u, true, all_caps()) == 1u,
          "Telit browse wraps after the final control page");
    check(netmon_adjacent_selector(96u, true, all_caps()) == 97u,
          "radio controls follow the local control pages");
    check(netmon_page_supported(netmon_registry_find(99u), all_caps()),
          "RF maintenance is visible after its fail-closed gate lands");
    check(!netmon_page_supported(netmon_registry_find(1u), local),
          "serving page unsupported without modem");
    check(netmon_page_supported(netmon_registry_find(40u), local),
          "battery page works without modem");
    check(netmon_page_supported(netmon_registry_find(72u), all_caps()),
          "runtime instrumentation pages are enabled");
    check(netmon_page_supported(netmon_registry_find(89u), no_modem_caps()),
          "backlight calibration is local and modem-independent");
    check(!netmon_page_supported(netmon_registry_find(28u), no_modem_caps()),
          "modem recovery page stays hidden without a modem backend");
    check(netmon_page_supported(netmon_registry_find(28u), all_caps()),
          "modem recovery page is available on RevB2/Telit");
}

static void test_provider_contract(void) {
    const netmon_page_descriptor_t *radio_state = netmon_registry_find(5u);
    const netmon_page_descriptor_t *search = netmon_registry_find(6u);
    const netmon_page_descriptor_t *tuner = netmon_registry_find(9u);
    const netmon_page_descriptor_t *voicemail = netmon_registry_find(36u);
    const netmon_page_descriptor_t *recovery = netmon_registry_find(28u);
    const netmon_page_descriptor_t *battery = netmon_registry_find(40u);
    const netmon_page_descriptor_t *scan_control = netmon_registry_find(97u);
    check(radio_state != NULL &&
              radio_state->refresh_group == NETMON_REFRESH_SERVING &&
              radio_state->refresh_period_ms == 5000u &&
              netmon_page_modem_group(radio_state) ==
                  MODEM_DIAG_GROUP_SERVING,
          "live radio state uses the bounded serving query group");
    check(search != NULL &&
              search->refresh_group == NETMON_REFRESH_RADIO_POLICY &&
              search->refresh_period_ms == 30000u &&
              search->stale_after_ms == 60000u,
          "policy readback refreshes before its cached payload expires");
    check(tuner != NULL &&
              tuner->refresh_group == NETMON_REFRESH_TUNER &&
              tuner->refresh_period_ms == 30000u &&
              tuner->stale_after_ms == 60000u,
          "tuner readback refreshes before its cached payload expires");
    check(voicemail != NULL &&
              voicemail->provider == NETMON_PROVIDER_MODEM_QUERY &&
              voicemail->refresh_group == NETMON_REFRESH_SMS_CONFIG &&
              voicemail->refresh_period_ms == 0u,
          "voicemail uses an explicit on-entry readback, not an empty cache");
    check(battery != NULL && battery->provider == NETMON_PROVIDER_LOCAL &&
              battery->refresh_group == NETMON_REFRESH_NONE &&
              netmon_page_modem_group(battery) == MODEM_DIAG_GROUP_NONE,
          "local battery page cannot wake the modem");
    check(recovery != NULL &&
              recovery->provider == NETMON_PROVIDER_MODEM_CACHE &&
              recovery->refresh_group == NETMON_REFRESH_NONE &&
              netmon_page_modem_group(recovery) == MODEM_DIAG_GROUP_NONE,
          "modem recovery counters use cache-only observation");
    check(scan_control != NULL &&
              scan_control->provider == NETMON_PROVIDER_MODEM_CACHE &&
              scan_control->refresh_group == NETMON_REFRESH_NONE &&
              netmon_page_modem_group(scan_control) == MODEM_DIAG_GROUP_NONE,
          "maintenance controls use their typed readback without a background query");
}

static void test_exact_key_normalization(void) {
    check(netmon_key_from_code(INPUT_KEY_POWER) == NETMON_KEY_POWER,
          "synthetic overlapping power code is exact");
    check(netmon_key_from_code(INPUT_KEY_NAVI) == NETMON_KEY_NAVI,
          "Navi is not confused with power");
    check(netmon_key_from_code(INPUT_KEY_STAR) == NETMON_KEY_STAR,
          "Star normalized");
    check(netmon_key_from_code(0xffffu) == NETMON_KEY_OTHER,
          "unknown chord is not treated as an edit key");
}

static void test_key_ownership(void) {
    const netmon_page_descriptor_t *read_only = netmon_registry_find(40u);
    const netmon_page_descriptor_t *editable = netmon_registry_find(95u);
    const netmon_key_t protected_keys[] = {
        NETMON_KEY_NAVI, NETMON_KEY_C, NETMON_KEY_POWER,
    };
    for (size_t page_index = 0u; page_index < netmon_registry_count(); page_index++) {
        const netmon_page_descriptor_t *page = netmon_registry_at(page_index);
        for (size_t i = 0u; i < sizeof(protected_keys) / sizeof(protected_keys[0]); i++) {
            check(netmon_key_disposition(page, NETMON_SURFACE_STANDBY, true,
                                         protected_keys[i]) == NETMON_KEY_PASS,
                  "Navi/C/Power always pass through");
        }
    }

    check(netmon_key_disposition(read_only, NETMON_SURFACE_STANDBY, true,
                                 NETMON_KEY_UP) == NETMON_KEY_BROWSE_PREVIOUS,
          "Up browses on standby");
    check(netmon_key_disposition(read_only, NETMON_SURFACE_STANDBY, true,
                                 NETMON_KEY_1) == NETMON_KEY_PASS,
          "read-only digit passes through");
    check(netmon_key_disposition(read_only, NETMON_SURFACE_STANDBY, false,
                                 NETMON_KEY_UP) == NETMON_KEY_PASS,
          "Up hold does not browse repeatedly");
    check(netmon_key_disposition(editable, NETMON_SURFACE_STANDBY, true,
                                 NETMON_KEY_1) == NETMON_KEY_EDIT,
          "declared edit key consumed on key-down");
    check(netmon_key_disposition(editable, NETMON_SURFACE_STANDBY, false,
                                 NETMON_KEY_1) == NETMON_KEY_CONSUME,
          "edit key hold is consumed without repeating the action");

    const netmon_page_descriptor_t *scan = netmon_registry_find(97u);
    const netmon_page_descriptor_t *antenna = netmon_registry_find(99u);
    check(netmon_key_disposition(scan, NETMON_SURFACE_STANDBY, true,
                                 NETMON_KEY_STAR) == NETMON_KEY_EDIT &&
              netmon_key_disposition(scan, NETMON_SURFACE_STANDBY, true,
                                     NETMON_KEY_7) == NETMON_KEY_EDIT &&
              netmon_key_disposition(scan, NETMON_SURFACE_STANDBY, true,
                                     NETMON_KEY_0) == NETMON_KEY_PASS,
          "scan page owns only arm/read and its seven presets");
    check(netmon_key_disposition(antenna, NETMON_SURFACE_STANDBY, true,
                                 NETMON_KEY_4) == NETMON_KEY_EDIT &&
              netmon_key_disposition(antenna, NETMON_SURFACE_STANDBY, true,
                                     NETMON_KEY_5) == NETMON_KEY_PASS,
          "antenna page cannot consume an undeclared number key");

    for (size_t page_index = 0u; page_index < netmon_registry_count(); page_index++) {
        const netmon_page_descriptor_t *page = netmon_registry_at(page_index);
        if ((page->behavior_flags & NETMON_PAGE_EDITABLE) == 0u) {
            continue;
        }
        for (unsigned key = NETMON_KEY_0; key <= NETMON_KEY_HASH; key++) {
            bool declared = (page->edit_keys & NETMON_EDIT_KEY(key)) != 0u;
            check(netmon_key_disposition(page, NETMON_SURFACE_STANDBY, true,
                                         (netmon_key_t)key) ==
                      (declared ? NETMON_KEY_EDIT : NETMON_KEY_PASS),
                  "editable page consumes exactly its declared keys");
            check(netmon_key_disposition(page, NETMON_SURFACE_STANDBY, false,
                                         (netmon_key_t)key) ==
                      (declared ? NETMON_KEY_CONSUME : NETMON_KEY_PASS),
                  "editable key holds are isolated to declared controls");
        }
    }

    const netmon_surface_t call_surfaces[] = {
        NETMON_SURFACE_CALL_OUTGOING,
        NETMON_SURFACE_CALL_CONNECTED,
        NETMON_SURFACE_CALL_INCOMING,
        NETMON_SURFACE_CALL_WAITING,
        NETMON_SURFACE_CALL_POPUP,
    };
    for (size_t s = 0u; s < sizeof(call_surfaces) / sizeof(call_surfaces[0]); s++) {
        for (unsigned key = NETMON_KEY_0; key <= NETMON_KEY_POWER; key++) {
            check(netmon_key_disposition(editable, call_surfaces[s], true,
                                         (netmon_key_t)key) == NETMON_KEY_PASS,
                  "all call-route keys pass through");
        }
    }
}

static void test_overlay_policy(void) {
    const netmon_page_descriptor_t *read_only = netmon_registry_find(1u);
    const netmon_page_descriptor_t *editable = netmon_registry_find(95u);
    check(netmon_overlay_visible(read_only, NETMON_SURFACE_STANDBY),
          "read-only visible on standby");
    check(netmon_overlay_visible(read_only, NETMON_SURFACE_CALL_OUTGOING),
          "read-only visible on outgoing call");
    check(netmon_overlay_visible(read_only, NETMON_SURFACE_CALL_CONNECTED),
          "read-only visible on connected call");
    check(!netmon_overlay_visible(read_only, NETMON_SURFACE_CALL_INCOMING),
          "incoming identity is unobscured");
    check(!netmon_overlay_visible(read_only, NETMON_SURFACE_CALL_WAITING),
          "waiting identity is unobscured");
    check(!netmon_overlay_visible(read_only, NETMON_SURFACE_CALL_POPUP),
          "call popups are unobscured");
    check(netmon_overlay_visible(editable, NETMON_SURFACE_STANDBY),
          "editable visible on standby");
    check(!netmon_overlay_visible(editable, NETMON_SURFACE_CALL_CONNECTED),
          "editable page suspended during calls");
}

int main(void) {
    test_registry();
    test_capability_browse();
    test_provider_contract();
    test_exact_key_normalization();
    test_key_ownership();
    test_overlay_policy();
    if (s_failures != 0) {
        fprintf(stderr, "%d Net Monitor logic test(s) failed\n", s_failures);
        return 1;
    }
    puts("Net Monitor logic tests passed");
    return 0;
}
