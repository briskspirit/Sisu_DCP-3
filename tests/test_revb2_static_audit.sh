#!/usr/bin/env bash
# Structural safety checks for the sole Rev B2/Telit product.
set -u

cd "$(dirname "$0")/.."

fail=0
scope=(
    BUILD.md CMakeLists.txt boards include src tests
    tools/tune_dynamic_ant
)
exclude_self=(--glob '!tests/test_revb2_static_audit.sh')

reject_pattern() {
    local pattern="$1"
    local description="$2"
    local matches status
    matches="$(rg -n -e "$pattern" "${exclude_self[@]}" "${scope[@]}" 2>&1)"
    status=$?
    case "$status" in
        0)
            echo "FAIL: $description"
            printf '%s\n' "$matches" | sed 's/^/    /'
            fail=1
            ;;
        1) ;;
        *)
            echo "FAIL: static-audit search failed while checking: $description"
            printf '%s\n' "$matches" | sed 's/^/    /'
            fail=1
            ;;
    esac
}

reject_pattern_except() {
    local pattern="$1"
    local description="$2"
    shift 2
    local matches status
    matches="$(rg -n -e "$pattern" "${exclude_self[@]}" "$@" "${scope[@]}" 2>&1)"
    status=$?
    case "$status" in
        0)
            echo "FAIL: $description"
            printf '%s\n' "$matches" | sed 's/^/    /'
            fail=1
            ;;
        1) ;;
        *)
            echo "FAIL: static-audit search failed while checking: $description"
            printf '%s\n' "$matches" | sed 's/^/    /'
            fail=1
            ;;
    esac
}

require_fixed() {
    local text="$1"
    local file="$2"
    local description="$3"
    if ! rg -q -F -- "$text" "$file"; then
        echo "FAIL: $description"
        fail=1
    fi
}

count_fixed_lines() {
    local text="$1"
    local file="$2"
    local count
    # rg returns 1 and emits no count for zero matches. Counting its matching
    # lines keeps zero a real numeric result instead of an empty shell value.
    count="$(rg -n -F -- "$text" "$file" 2>/dev/null | wc -l | tr -d '[:space:]')"
    printf '%s\n' "${count:-0}"
}

require_fixed_count_at_least() {
    local text="$1"
    local file="$2"
    local minimum="$3"
    local description="$4"
    local count
    count="$(count_fixed_lines "$text" "$file")"
    if [ "$count" -lt "$minimum" ]; then
        echo "FAIL: $description (found $count, need at least $minimum)"
        fail=1
    fi
}

# A removed scope root must fail before any denylist runs. Otherwise every
# reject_pattern search sees rg's exit 2 and can silently look like "no match."
for path in "${scope[@]}"; do
    if [ ! -e "$path" ]; then
        echo "FAIL: static-audit scope path is missing: $path"
        fail=1
    fi
done

# Pin reject_pattern's tri-state contract itself: 0=forbidden match, 1=clean,
# >1=search failure. This probe specifically recreates the missing-scope bug.
static_audit_probe_root="$(mktemp -d)"
trap 'rm -rf "$static_audit_probe_root"' EXIT
if ! (
    scope=("$static_audit_probe_root/intentionally-missing")
    fail=0
    reject_pattern 'never-matches' 'missing-scope regression probe'
    [ "$fail" -eq 1 ]
) >/dev/null 2>&1; then
    echo "FAIL: reject_pattern does not fail closed on rg search errors"
    fail=1
fi
if ! (
    scope=("$static_audit_probe_root/intentionally-missing")
    fail=0
    reject_pattern_except 'never-matches' 'missing-scope regression probe' \
        --glob '!nothing'
    [ "$fail" -eq 1 ]
) >/dev/null 2>&1; then
    echo "FAIL: reject_pattern_except does not fail closed on rg search errors"
    fail=1
fi

# Keep the count primitive total at the exact boundary that previously let a
# deleted invariant false-pass: no match, one match, and the required two.
if [ "$(printf '' | count_fixed_lines needle -)" != "0" ] ||
   [ "$(printf 'needle\n' | count_fixed_lines needle -)" != "1" ] ||
   [ "$(printf 'needle\nneedle\n' | count_fixed_lines needle -)" != "2" ]; then
    echo "FAIL: static-audit fixed-line counter is not total for 0/1/2 matches"
    fail=1
fi

reject_pattern \
    'MODEM_PIN_3V8_KILL|BATTERY_ADC_(PIN|INPUT)|TCA8418_PIN_(ROW6|COL9)' \
    "discontinued board pin symbols remain in production/test code"
reject_pattern \
    'SISU_HW_REV_B1|SISU_MODEM_VENDOR_UBLOX|MODEM_WAKE_TX_POKE|MODEM_DEGRADE_UART_IDLE' \
    "discontinued board/backend selectors remain in production/test code"
reject_pattern \
    '(LARA-R6|u-blox|modem_vendor_ublox|call_timing_lara|sisu_sse_revb1)' \
    "discontinued modem or board identity remains in production/test code"
reject_pattern \
    '(MODEM_CALL_MODEL_SHADOW|modem_call_shadow|modem_service_shadow_use_new|modem_service_set_call_model_publish|publish_table_by_default)' \
    "retired call-model migration shadow remains in production/test code"
reject_pattern \
    '(MODEM_CALL_SETUP_TIMEOUT_MS|MODEM_CLCC_RECONCILE_MS|MODEM_ABANDON_GUARD_MS|MODEM_RELEASE_SURVIVOR_GUARD_MS|MODEM_AT_CLCC_RECONCILE|s_(active_call_id|ringing_call_id|held_call_id|second_mo_id|second_mo_held_survivor_id|abandoned_call_id|new_call_cleanup_id|release_survivor_id)|set_held_call_id|legacy_call_command_dispatched|finish_legacy_clcc_reconcile)' \
    "retired split-brain call authority was reintroduced"
reject_pattern \
    'CORE1_CMD_AUDIO_VIBRA_ENABLE' \
    "retired two-command marker-vibra arm protocol remains in production/test code"
reject_pattern \
    'modem_service_request_dtmf[[:space:]]*\([[:space:]]*app->editor_value\[' \
    "the DTMF editor reverted to one FIFO request per character"

tracked_discontinued_refs="$(git grep -nE \
    'LARA|u-blox|ublox|RevA|Rev A|rev A|RevB1|Rev B1|rev B1|SISU_HW_REV_B1|SISU_MODEM_VENDOR_UBLOX' \
    -- ':!tests/test_revb2_static_audit.sh' || true)"
if [ -n "$tracked_discontinued_refs" ]; then
    echo "FAIL: discontinued board or modem identity remains in a tracked file"
    printf '%s\n' "$tracked_discontinued_refs" | sed 's/^/    /'
    fail=1
fi
reject_pattern \
    'PICO_RP2350A[[:space:]]+1' \
    "Rev B2 must select the 48-GPIO RP2350B package"
reject_pattern \
    'sio_hw->gpio_(oe|out)' \
    "raw SIO output/enable access bypasses the GPIO ownership layer"
reject_pattern_except \
    '"AT#' \
    "Telit commands escaped the Telit backend, diagnostic, or its host test" \
    --glob '!src/diag/diag_telit.c' \
    --glob '!tools/tune_dynamic_ant/main.c' \
    --glob '!src/services/modem_vendor_telit*.c' \
    --glob '!tests/test_modem_vendor_telit.c' \
    --glob '!tests/test_modem_telit_service.c'
reject_pattern_except \
    'gpio_put[[:space:]]*\([[:space:]]*MODEM_PIN_(ON_OFF|HW_SHUTDOWN)[[:space:]]*,[[:space:]]*true[[:space:]]*\)' \
    "a Telit control assertion escaped its HAL or standalone diagnostic" \
    --glob '!src/hal/modem_uart_hal.c' \
    --glob '!src/diag/diag_telit.c' \
    --glob '!tools/tune_dynamic_ant/main.c'

if rg -q -F 'SISU_TELIT_TUNER_DIAGNOSTIC' src/diag/diag_telit.c; then
    echo "FAIL: dynamic-antenna tuner code leaked back into generic diag_telit"
    fail=1
fi
discontinued_paths="$(rg --files boards include src tests tools/tune_dynamic_ant | rg -i \
    '(^|/)(sisu_sse\.h|.*revb1.*|.*ublox.*|.*lara.*)$' || true)"
if [ -n "$discontinued_paths" ]; then
    echo "FAIL: discontinued board/backend files remain"
    printf '%s\n' "$discontinued_paths" | sed 's/^/    /'
    fail=1
fi
retired_shadow_paths="$(rg --files include src tests | rg 'modem_call_shadow' || true)"
if [ -n "$retired_shadow_paths" ]; then
    echo "FAIL: retired call-model migration shadow files remain"
    printf '%s\n' "$retired_shadow_paths" | sed 's/^/    /'
    fail=1
fi

legacy_call_status_access="$(rg -n \
    's_status\.(call_state|active_call_id|call_on_hold|second_call_held|waiting_call|ring_active|caller_id_withheld|incoming_diverted|incoming_number|last_call_result|second_call_result)' \
    src/services/modem_service.c || true)"
if [ -n "$legacy_call_status_access" ]; then
    echo "FAIL: modem service reads or writes the retired flat call authority"
    printf '%s\n' "$legacy_call_status_access" | sed 's/^/    /'
    fail=1
fi

callback_files="$(rg -l -F 'gpio_set_irq_enabled_with_callback' \
    src include tests "${exclude_self[@]}" || true)"
if [ "$callback_files" != "src/hal/board_irq_hal.c" ]; then
    echo "FAIL: board_irq_hal.c must be the sole core-global GPIO callback owner"
    printf '    owners: %s\n' "${callback_files:-<none>}"
    fail=1
fi

unexpected_rail_on="$(rg -n -F 'board_3v8_rail_set_enabled(true)' src \
    --glob '!src/services/shared_3v8_service.c' \
    --glob '!src/diag/diag_revb2_power.c' \
    --glob '!src/diag/diag_telit.c' || true)"
if [ -n "$unexpected_rail_on" ]; then
    echo "FAIL: unexpected direct +3V8 enable path"
    printf '%s\n' "$unexpected_rail_on" | sed 's/^/    /'
    fail=1
fi
unexpected_production_rail_control="$(rg -n -F 'board_3v8_rail_set_enabled(' \
    src/apps src/services src/main.c \
    --glob '!src/services/shared_3v8_service.c' || true)"
if [ -n "$unexpected_production_rail_control" ]; then
    echo "FAIL: production +3V8 control bypasses shared ownership"
    printf '%s\n' "$unexpected_production_rail_control" | sed 's/^/    /'
    fail=1
fi

# UART0's NVIC vector is core-global. The modem service must claim it during
# single-core startup, before core 1 installs/uses any IRQ-backed service. This
# ordering is a regression guard for the production-only Telit bring-up race;
# the standalone diagnostic is single-core and cannot expose it.
modem_init_line="$(rg -n -F 'modem_service_init();' src/main.c | head -n 1 | cut -d: -f1)"
core1_start_line="$(rg -n -F 'core1_services_start(' src/main.c | head -n 1 | cut -d: -f1)"
if [ -z "$modem_init_line" ] || [ -z "$core1_start_line" ] ||
   [ "$modem_init_line" -ge "$core1_start_line" ]; then
    echo "FAIL: modem service must claim UART IRQ ownership before core 1 starts"
    fail=1
fi

irq_claim_text='irq_set_exclusive_handler(uart_irq, modem_uart_rx_isr);'
irq_claim_count="$(count_fixed_lines "$irq_claim_text" src/hal/modem_uart_hal.c)"
uart_init_line="$(rg -n '^void modem_uart_hal_init\(void\)' src/hal/modem_uart_hal.c | head -n 1 | cut -d: -f1)"
irq_claim_line="$(rg -n -F "$irq_claim_text" src/hal/modem_uart_hal.c | head -n 1 | cut -d: -f1)"
irq_arm_line="$(rg -n '^static void modem_uart_hal_arm_rx_irq\(void\)' src/hal/modem_uart_hal.c | head -n 1 | cut -d: -f1)"
if [ "$irq_claim_count" != "1" ] || [ -z "$uart_init_line" ] ||
   [ -z "$irq_claim_line" ] || [ -z "$irq_arm_line" ] ||
   [ "$irq_claim_line" -le "$uart_init_line" ] ||
   [ "$irq_claim_line" -ge "$irq_arm_line" ]; then
    echo "FAIL: UART RX handler must be claimed exactly once in early HAL init"
    fail=1
fi

require_fixed \
    'set(PICO_BOARD sisu_sse_revb2' CMakeLists.txt \
    "CMake must select the sole Rev B2 board directly"
require_fixed \
    'set(SISU_FIRMWARE_TARGET sisu_dcp3_revb2_telit)' CMakeLists.txt \
    "CMake must expose one fixed Rev B2/Telit production target"
require_fixed \
    'option(RELEASE' CMakeLists.txt \
    "CMake must expose the explicit no-CDC release profile"
require_fixed \
    'SISU_RELEASE_BUILD=${SISU_RELEASE_BUILD_VALUE}' CMakeLists.txt \
    "the firmware composition root must publish its release profile to C"
require_fixed \
    'OUTPUT_NAME "${SISU_FIRMWARE_TARGET}_release"' CMakeLists.txt \
    "the release artifact must be visibly distinct from the service image"
require_fixed \
    'pico_enable_stdio_usb(${SISU_FIRMWARE_TARGET} 0)' CMakeLists.txt \
    "the release profile must disable SDK USB stdio"
require_fixed \
    'cmake/check_release_image.cmake' CMakeLists.txt \
    "the release link must enforce the CDC/service symbol denylist"
debug_console_source_count="$(count_fixed_lines \
    'src/services/debug_console.c' CMakeLists.txt)"
if [ "$debug_console_source_count" != "1" ]; then
    echo "FAIL: debug_console.c must exist only as the conditional service-profile source"
    fail=1
fi
require_fixed \
    'src/services/modem_vendor_telit.c' CMakeLists.txt \
    "production must compose the Telit backend directly"
require_fixed \
    'SISU_HW_REV_B2=1' CMakeLists.txt \
    "all firmware targets must identify Rev B2"
require_fixed \
    'SISU_MODEM_VENDOR_TELIT=1' CMakeLists.txt \
    "the production image must identify Telit at the composition root"
require_fixed \
    'SISU_MODEM_BACKEND_ENABLED=1' CMakeLists.txt \
    "the product must expose the neutral enabled-backend capability"
require_fixed \
    'model_apply_projection(out, &s_call_projection);' src/services/modem_service.c \
    "public call status must come from the id-authoritative call model"
require_fixed \
    '#define CALL_DIVERT_REQUEST_RECORD_ID 0x25u' \
    include/apps/call_divert_app.h \
    "call-divert Requesting must retain the v6.00 display-message record"
require_fixed \
    'app->display_record_id == CALL_DIVERT_REQUEST_RECORD_ID' \
    src/apps/dialogs_app.c \
    "the Requesting dialog must expose its v6.00 Quit softkey"
require_fixed \
    '{CALL_DIVERT_REQUEST_RECORD_ID,' src/apps/dialogs_app.c \
    "the Requesting dialog must use its v6.00 persistent progress metadata"
require_fixed \
    'app->display_record_id == CALL_DIVERT_DETAIL_RECORD_ID' \
    src/apps/dialogs_app.c \
    "v6.00 call-divert detail record 0x20 must retain its Back softkey"
require_fixed \
    'modem_call_state_t state = s_call_projection.call_state;' \
    src/services/modem_service.c \
    "call audio decisions must come from the id-authoritative call model"
require_fixed \
    'modem_service_request_dtmf_sequence(app->editor_value)' \
    src/apps/dialogs_app.c \
    "the DTMF editor must admit its whole string atomically"
require_fixed \
    'SISU_MODEM_VENDOR_NONE=1' CMakeLists.txt \
    "modem-free diagnostics must retain the inert NONE identity"
require_fixed \
    'SISU_MODEM_BACKEND_ENABLED=0' CMakeLists.txt \
    "modem-free diagnostics must disable the neutral HAL capability"
require_fixed \
    '#define MODEM_STATUS_MONITOR_AVAILABLE 1u' include/hal/board.h \
    "Rev B2 must expose its fitted GP40 status monitor"
require_fixed \
    'src/services/shared_3v8_service.c' CMakeLists.txt \
    "production must compose the shared +3V8 owner service"
require_fixed \
    'add_executable(diag_telit' CMakeLists.txt \
    "the Telit characterization tool must remain a standalone target"
require_fixed \
    'add_executable(diag_telit_dvi' CMakeLists.txt \
    "the Telit DVI/OAP gate must remain a standalone diagnostic target"
require_fixed \
    'add_subdirectory(tools/tune_dynamic_ant)' CMakeLists.txt \
    "manual antenna tuning must be isolated under tools"
require_fixed \
    'add_executable(tune_dynamic_ant' tools/tune_dynamic_ant/CMakeLists.txt \
    "manual antenna tuning must remain a standalone diagnostic target"
require_fixed \
    'SISU_TELIT_TUNER_DIAGNOSTIC=1' tools/tune_dynamic_ant/CMakeLists.txt \
    "antenna-tuner controls must require an explicit target-only build flag"
require_fixed \
    'AT+CFUN=4' tools/tune_dynamic_ant/main.c \
    "the tuner diagnostic must request a TX/RX-disabled modem state"
require_fixed \
    'AT#GPIO=2,%u,1,0' tools/tune_dynamic_ant/main.c \
    "manual tuner GPIO writes must remain non-persistent"
require_fixed \
    's_tuner_cfun != 4u' tools/tune_dynamic_ant/main.c \
    "the tuner diagnostic must verify RF-off before publishing READY"
require_fixed \
    'SISU_TELIT_DVI_DIAGNOSTIC=1' CMakeLists.txt \
    "DVI/OAP commands must require an explicit target-only build flag"
require_fixed \
    'telit_dvi_diag_tick(timestamp_ms);' src/diag/diag_telit.c \
    "the DVI diagnostic must retain its qualified-start and clock-loss tick"
require_fixed \
    '#define TELIT_DVI_BCLK_STABLE_MS 20u' src/diag/telit_dvi_diag.c \
    "the DVI diagnostic must qualify continuously running BCLK before bridging"
require_fixed \
    '#define TELIT_DIAG_OFF_THRESHOLD_MAX_RAW 1024u' \
    include/diag/telit_diag_logic.h \
    "Telit rail cut must retain its conservative uncalibrated ADC guardrail"
require_fixed \
    '#define TELIT_DIAG_EMERGENCY_ARM_MS 5000u' \
    include/diag/telit_diag_logic.h \
    "emergency shutdown must retain its short two-step arm window"
require_fixed \
    '#define TELIT_DIAG_OFF_STABLE_MS 500u' \
    include/diag/telit_diag_logic.h \
    "rail cut must require a continuous low PWRMON interval"
require_fixed \
    '#define TELIT_DIAG_STATUS_SAMPLE_MAX_GAP_MS 50u' \
    include/diag/telit_diag_logic.h \
    "stale ADC data must not satisfy the continuous-low interlock"
require_fixed \
    'add_alarm_in_ms(' tools/tune_dynamic_ant/main.c \
    "Telit control pulses require a hardware-timer deassertion backstop"
require_fixed \
    'add_alarm_in_ms(' src/hal/modem_uart_hal.c \
    "production Telit shutdown pulses require an alarm-backed release"
require_fixed \
    's_shutdown_pulse_release_ms = now_ms + width_ms + 1u;' \
    src/services/modem_service.c \
    "the coarse service deadline must never shorten a hardware-owned pulse"
require_fixed \
    'RO_LOCAL(52u, NETMON_CATEGORY_BOARD)' src/apps/net_monitor/logic.c \
    "Rev B2 shared-IRQ summary must remain a registry-owned local page"
require_fixed \
    'RO_LOCAL(53u, NETMON_CATEGORY_BOARD)' src/apps/net_monitor/logic.c \
    "Rev B2 shared-IRQ evidence must remain a registry-owned local page"
netmon_app_leaks="$(rg -n \
    '^#include "(hal/|audio/|services/core1_services|services/modem_vendor)' \
    src/apps/net_monitor/app.c || true)"
if [ -n "$netmon_app_leaks" ]; then
    echo "FAIL: Net Monitor app shell bypasses its service/renderer boundary"
    printf '%s\n' "$netmon_app_leaks" | sed 's/^/    /'
    fail=1
fi
if rg -q -e 'g_modem_vendor|MODEM_NETMON_VENDOR' src/apps/net_monitor/app.c; then
    echo "FAIL: Net Monitor app shell depends on a modem vendor surface"
    fail=1
fi
require_fixed \
    'SHARED_3V8_OWNER_MODEM, true' src/services/modem_service.c \
    "the modem must acquire +3V8 through shared ownership"
require_fixed \
    'shared_3v8_service_transition_count()' src/services/modem_service.c \
    "modem discharge dwell must detect an interleaved buzzer rail pulse"
require_fixed \
    'SHARED_3V8_OWNER_AUDIO, true' src/services/core1_services.c \
    "buzzer audio must acquire +3V8 before command publication"
require_fixed \
    'audio_service_command_uses_buzzer(cmd, arg)' src/services/core1_services.c \
    "core0 rail ownership must follow the canonical audio route predicate"
require_fixed \
    'gpio_put(BUZZER_PIN, BUZZER_ACTIVE_LOW != 0u);' src/audio/buzzer_hal.c \
    "the buzzer idle clamp must drive the configured inactive polarity"
if ! rg -q -U \
    'pwm_set_enabled\(s_slice, false\);\n[[:space:]]*buzzer_clamp_inactive\(\);' \
    src/audio/buzzer_hal.c; then
    echo "FAIL: buzzer off must disable PWM and remux the gate to its SIO clamp"
    fail=1
fi
require_fixed \
    's_command_dispatching = true;' src/services/core1_services.c \
    "core1 dequeue must publish an in-flight command before releasing the queue lock"
require_fixed \
    '!s_command_dispatching &&' src/services/core1_services.c \
    "audio +3V8 release must reject the dequeue-to-dispatch handoff window"
require_fixed \
    '!shared_3v8_service_enabled()' src/services/power_sleep.c \
    "dormant entry must wait for every +3V8 owner to release"
require_fixed \
    'shared_3v8_service_owner_mask() == 0u' src/services/power_sleep.c \
    "dormant entry must reject logically-owned +3V8 even if PG is absent"
require_fixed \
    'shared_3v8_service_force_off();' src/services/power_sleep.c \
    "the P1.7 point of no return must force shared +3V8 down"
require_fixed \
    '#if !SISU_MODEM_BACKEND_ENABLED' src/hal/modem_uart_hal.c \
    "UART HAL must compile inert paths when no backend is enabled"
require_fixed \
    'return true; /* NONE has no modem UART work in flight. */' src/hal/modem_uart_hal.c \
    "NONE clock-down predicate must not read an uninitialized modem UART"
require_fixed \
    '.available = false' src/services/modem_vendor_none.c \
    "NONE backend must remain unavailable"
require_fixed \
    '.capabilities = 0u' src/services/modem_vendor_none.c \
    "NONE backend must keep the modem voice transport disabled"
require_fixed \
    '.available = true' src/services/modem_vendor_telit.c \
    "the bench-gated Telit backend must advertise availability"
require_fixed \
    '.capabilities = MODEM_VENDOR_CAP_DTR_SLEEP |' src/services/modem_vendor_telit.c \
    "Telit must advertise the bench-qualified DTR and DVI transports"
require_fixed \
    'core1_services_start(modem_service_voice_transport_available())' src/main.c \
    "core1 audio must receive the selected backend voice capability"
require_fixed \
    'bool modem_was_active = modem_i2s_hal_running();' src/services/core1_services.c \
    "flash parking must cover modem DMA during pre-bridge BCLK qualification"
require_fixed \
    's_bridge_start_pending || modem_i2s_hal_running();' src/audio/audio_service.c \
    "an incompletely stopped modem transport must block low-power entry"
require_fixed \
    '#define BOARD_QUIET_CLOCK_HZ 6000000u' src/hal/board.c \
    "quiet clk_sys/clk_peri must use the 6 MHz Phase-B operating point"
require_fixed \
    'BOARD_XOSC_CLOCK_HZ, BOARD_QUIET_CLOCK_HZ);' src/hal/board.c \
    "quiet clock transitions must preserve the 12 MHz source and divide to 6 MHz"
require_fixed \
    'src/hal/standby_sleep_hal.c' CMakeLists.txt \
    "production must compose the powered-on dormant HAL"
require_fixed \
    'src/services/standby_sleep.c' CMakeLists.txt \
    "production must compose the powered-on dormant policy"
require_fixed \
    'standby_sleep_try_enter(' src/main.c \
    "the product loop must hand eligible standby to clock-DORMANT"
require_fixed \
    '{MODEM_PIN_RI, GPIO_IRQ_EDGE_FALL' src/hal/standby_sleep_hal.c \
    "Telit RI must wake powered-on dormant on its falling edge"
require_fixed \
    '{SYS_INT_PIN, GPIO_IRQ_EDGE_FALL' src/hal/standby_sleep_hal.c \
    "the shared keypad/RTC line must wake powered-on dormant"
require_fixed \
    '{POWER_BUTTON_PIN, GPIO_IRQ_EDGE_FALL' src/hal/standby_sleep_hal.c \
    "the power button must wake powered-on dormant"
require_fixed \
    '{SERVICE_VBUS_PIN, GPIO_IRQ_EDGE_RISE' src/hal/standby_sleep_hal.c \
    "service VBUS must wake powered-on dormant"
require_fixed \
    'powman_enable_alarm_wakeup_at_ms(alarm_ms);' src/hal/standby_sleep_hal.c \
    "powered-on dormant must retain its absolute AON maintenance wake"
require_fixed \
    'timebase_rebase_ms(result.wall_time_ms);' src/services/standby_sleep.c \
    "dormant restore must rebase from the authoritative continuous AON wall clock"
require_fixed \
    'modem_uart_hal_cts_asserted()) {' src/hal/standby_sleep_hal.c \
    "the interrupt-off dormant arm point must recheck modem CTS and UART idleness"
require_fixed \
    'if (!modem_uart_hal_clock_change_begin()) {' src/hal/standby_sleep_hal.c \
    "powered-on dormant must deassert RTS before removing the modem UART clock"
require_fixed \
    'modem_uart_hal_clock_change_end();' src/hal/standby_sleep_hal.c \
    "powered-on dormant must restore modem RX flow after UART clock recovery"
require_fixed \
    'MODEM_UART_RX_FLOW_PAUSE' src/hal/modem_uart_hal.c \
    "the modem RX ISR must backpressure Telit before its software ring fills"
require_fixed \
    'modem_service_transport_sleep_confirmed()' src/services/standby_sleep.c \
    "dormant policy must require a confirmed modem DTR/CTS boundary"
require_fixed \
    'core1_services_standby_ready()' src/services/standby_sleep.c \
    "dormant policy must require core1 audio and DMA quiescence"
require_fixed \
    'message_service_sleep_ready()' src/services/standby_sleep.c \
    "standby must allow retained SMS queues deferred by storage failures"
require_fixed \
    'message_service_sleep_ready()' src/services/power_sleep.c \
    "deep power-off must not wait forever for an unwritable SMS queue"
require_fixed \
    'irq_set_exclusive_handler(DMA_IRQ_1, modem_dma_irq1_handler);' src/audio/modem_i2s_hal.c \
    "modem DMA must not consume or exhaust the global shared-IRQ handler pool"
reject_pattern \
    'irq_add_shared_handler[[:space:]]*\([[:space:]]*DMA_IRQ_1' \
    "modem DMA IRQ1 must remain exclusively owned"
require_fixed \
    '__no_inline_not_in_flash_func(core1_flash_park)' src/services/core1_services.c \
    "the flash park must remain out-of-line in SRAM under Release optimization"
require_fixed \
    '#define POWER_SLEEP_LAST_PWRUP_BUTTON_BITS (1u << 1)' src/hal/power_sleep_wake_logic.c \
    "bench-proven PWRUP0/button LAST_SWCORE bit must remain 0x02"
require_fixed \
    '#define POWER_SLEEP_LAST_PWRUP_SHARED_IRQ_BITS (1u << 2)' src/hal/power_sleep_wake_logic.c \
    "bench-proven PWRUP1/shared-IRQ LAST_SWCORE bit must remain 0x04"
require_fixed \
    '#define POWER_SLEEP_LAST_PWRUP_SERVICE_VBUS_BITS (1u << 3)' src/hal/power_sleep_wake_logic.c \
    "PWRUP2/service-VBUS LAST_SWCORE bit must remain 0x08"
require_fixed \
    'powman_disable_all_wakeups()' src/hal/power_sleep_hal.c \
    "dormant wake ownership must clear stale GPIO slots and the POWMAN alarm"
require_fixed \
    'board_set_sleep_gating(false);' src/services/power_sleep.c \
    "P1.7 entry must clear the ordinary-WFI SLEEP_EN mask"
require_fixed \
    'battery_hal_charger_input_present_now(time_ms())' src/services/power_sleep.c \
    "dormant commit must freshly veto a full-pack charger on GP43"
require_fixed \
    'rv8803_hal_stop_periodic_timer()' src/services/power_sleep.c \
    "boot must retire a periodic timer inherited from older firmware"
require_fixed \
    'POWER_SLEEP_ABORT_LEGACY_TIMER_STOP' src/services/power_sleep.c \
    "a failed legacy-timer cleanup must veto dormant commit"
require_fixed \
    'rtc_alarm_hal_alarm_config_committed()' src/services/power_sleep.c \
    "P1.7 must not discard a snooze/alarm configuration still pending in RP RAM"
require_fixed \
    'rtc_alarm_hal_datetime_write_pending()' src/services/power_sleep.c \
    "P1.7 must not discard a wall-clock write still pending in RP RAM"
require_fixed \
    'rtc_alarm_hal_snooze_active()' src/app_status_runtime.c \
    "a tagged snooze recovered after P1.7 must restore the Snooze active UI state"
if rg -q -F 'rv8803_hal_start_periodic_timer' src/services/power_sleep.c; then
    echo "FAIL: production P1.7 must not arm the retired charger heartbeat"
    fail=1
fi
if ! rg -q -U \
    'powman_enable_gpio_wakeup\([[:space:]]*POWER_SLEEP_BUTTON_WAKE_SLOT,[[:space:]]*POWER_BUTTON_PIN,[[:space:]]*true,[[:space:]]*false[[:space:]]*\)' \
    src/hal/power_sleep_hal.c; then
    echo "FAIL: power-button dormant wake must remain falling-edge triggered"
    fail=1
fi
if ! rg -q -U \
    'powman_enable_gpio_wakeup\([[:space:]]*POWER_SLEEP_SHARED_WAKE_SLOT,[[:space:]]*SYS_INT_PIN,[[:space:]]*true,[[:space:]]*false[[:space:]]*\)' \
    src/hal/power_sleep_hal.c; then
    echo "FAIL: shared-IRQ dormant wake must remain falling-edge triggered"
    fail=1
fi
if ! rg -q -U \
    'powman_enable_gpio_wakeup\([[:space:]]*POWER_SLEEP_VBUS_WAKE_SLOT,[[:space:]]*SERVICE_VBUS_PIN,[[:space:]]*true,[[:space:]]*true[[:space:]]*\)' \
    src/hal/power_sleep_hal.c; then
    echo "FAIL: service-VBUS dormant wake must remain rising-edge triggered"
    fail=1
fi
require_fixed \
    '#define LTC2959_SAMPLE_FAIL_LIMIT 3u' src/hal/ltc2959_hal.c \
    "LTC2959 scheduled reads must have an independent stale-sample limit"
require_fixed \
    'tca8418_hal_get_charger_enabled(&actual)' \
    src/services/charger_control_service.c \
    "the charger arbiter must verify COL5 readback, not publish a software latch"
require_fixed \
    'board_diag_restore_charger_default();' src/apps/power_app.c \
    "soft-off must clear only its diagnostic charger-disable owner"
require_fixed \
    'battery_raw_collapse_evidence_update(' src/app_status_runtime.c \
    "raw-collapse shutdown must require distinct LTC samples"
require_fixed \
    'battery_empty_shutdown_threshold_mv(call_active)' src/app_status_runtime.c \
    "ordinary EMPTY must retain a separate in-call emergency floor"
require_fixed \
    'battery_endpoint_step(' src/app_status_runtime.c \
    "fused endpoint policy must own EMPTY, sag, and emergency decisions"
require_fixed \
    'modem_service_take_supply_power_failure()' src/app_status_runtime.c \
    "modem supply-PG failure must reach the battery-empty safety path"
require_fixed \
    'board_diag_battery_supply_failure_indicates_empty()' \
    src/app_status_runtime.c \
    "supply-PG failure must require independent marginal-pack evidence"
require_fixed \
    '.anchored_capacity_has_charge = anchored_capacity_has_charge' \
    src/app_status_runtime.c \
    "anchored remaining charge must participate in load-sag classification"
require_fixed \
    'BATTERY_WARNING_ACTION_HOLD' src/app_status_runtime.c \
    "a failed LTC conversion must preserve active LOW/EMPTY warning state"
require_fixed \
    'battery_display_bars(' src/app_status_runtime.c \
    "a transient invalid LTC sample must retain the last qualified icon"
require_fixed \
    'uint32_t learning_events = completion.learn_full' \
    src/app_status_runtime.c \
    "only supervisor-qualified completion may reach the learner's full anchor"
require_fixed \
    'now_ms, BATTERY_LEARNING_EVENT_EMPTY,' src/app_status_runtime.c \
    "voltage-accepted EMPTY must close a capacity cycle"
require_fixed \
    'uint32_t learning_result = battery_learning_service_poll(' \
    src/app_status_runtime.c \
    "the app must consume learner authority results"
require_fixed \
    '.capacity_prediction_exhausted =' src/app_status_runtime.c \
    "capacity exhaustion must remain an explicit unconfirmed prediction"
if rg -q 'BATTERY_LEARNING_RESULT_CAPACITY_EMPTY' include src; then
    echo "FAIL: estimated capacity exhaustion regained EMPTY authority"
    fail=1
fi
require_fixed \
    'if (endpoint.learn_empty)' src/app_status_runtime.c \
    "only the fused physical endpoint may teach an EMPTY capacity sample"
require_fixed \
    'evidence->call_active && evidence->gauge_empty' \
    src/apps/battery_status_logic.c \
    "in-call gauge EMPTY must pass through load-sag classification"
require_fixed \
    '!evidence->capacity_prediction_exhausted' \
    src/apps/battery_status_logic.c \
    "only positive charge below an unexhausted anchored estimate may suppress sag"
require_fixed \
    'bool corroborated_capacity_endpoint =' \
    src/apps/battery_status_logic.c \
    "capacity exhaustion and committed LOW must fuse before the idle hard floor"
require_fixed \
    '!evidence->gauge_empty && !evidence->call_active' \
    src/apps/battery_status_logic.c \
    "capacity-plus-LOW EMPTY must not convert active call sag into an endpoint"
require_fixed \
    'learner.model.soc_bars_valid, learner.model.soc_bars' \
    src/app_status_runtime.c \
    "qualified learner SOC must own the displayed battery bars"
require_fixed \
    'battery_charge_supervisor_service_effective_charge_factor_permille()' \
    src/app_status_runtime.c \
    "learner charge projection must use the supervisor's effective factor"
require_fixed \
    'battery_charge_supervisor_service_poll(now_ms);' \
    src/app_status_runtime.c \
    "the charge supervisor must run before the learner advances its SOC ledger"
require_fixed \
    'BATTERY_CHARGE_SUPERVISOR_RESULT_FULL_QUALIFIED' \
    src/app_status_runtime.c \
    "every qualified hardware or software FULL must reach the learner"
require_fixed \
    'battery_anchored_remaining_has_charge(' src/app_status_runtime.c \
    "load-sag protection must not depend on display-bar validity"
require_fixed \
    'battery_charge_supervisor_service_release_for_battery_floor()' \
    src/app_status_runtime.c \
    "attached LOW/EMPTY must release the supervisor's own charger inhibit"
require_fixed \
    'battery_full_notice_step(' src/app_status_runtime.c \
    "an attached maintenance generation must re-arm its qualified FULL notice"
supervisor_poll_line="$(rg -n -F \
    'battery_charge_supervisor_service_poll(now_ms);' \
    src/app_status_runtime.c | head -n 1 | cut -d: -f1)"
learner_poll_line="$(rg -n -F \
    'uint32_t learning_result = battery_learning_service_poll(' \
    src/app_status_runtime.c | head -n 1 | cut -d: -f1)"
if [ -z "$supervisor_poll_line" ] || [ -z "$learner_poll_line" ] ||
        [ "$supervisor_poll_line" -ge "$learner_poll_line" ]; then
    echo "FAIL: charge admission must freeze learner evidence before the learner poll"
    fail=1
fi
endpoint_poll_line="$(rg -n -F \
    'now_ms, BATTERY_LEARNING_EVENT_EMPTY,' \
    src/app_status_runtime.c | head -n 1 | cut -d: -f1)"
endpoint_flush_line="$(rg -n -F \
    '(void)store_service_flush_all();' \
    src/app_status_runtime.c | cut -d: -f1 | \
    awk -v start="$endpoint_poll_line" '$1 > start { print; exit }')"
empty_notice_line="$(rg -n -F \
    'app, 15u, 0x259u' \
    src/app_status_runtime.c | head -n 1 | cut -d: -f1)"
if [ -z "$endpoint_poll_line" ] || [ -z "$endpoint_flush_line" ] ||
        [ -z "$empty_notice_line" ] ||
        [ "$endpoint_poll_line" -ge "$endpoint_flush_line" ] ||
        [ "$endpoint_flush_line" -ge "$empty_notice_line" ]; then
    echo "FAIL: EMPTY endpoint must be learned and flushed before its notice"
    fail=1
fi
if rg -q -e 'charger_control_service_set_inhibit|board_diag_debug_set_charger_enabled' \
        src/services/battery_charge_supervisor_logic.c; then
    echo "FAIL: pure charge-supervisor logic acquired charger-control authority"
    fail=1
fi
if rg -q 'board_diag_debug_set_charger_enabled' \
        src/services/battery_charge_supervisor_service.c; then
    echo "FAIL: charge supervisor bypasses the additive charger-control owner"
    fail=1
fi
require_fixed \
    'supervisor_store_durable()' \
    src/services/battery_charge_supervisor_service.c \
    "the charge supervisor must prove its stop latch durable before /CE control"
require_fixed \
    'CHARGER_INHIBIT_SUPERVISOR, inhibit' \
    src/services/battery_charge_supervisor_service.c \
    "charge enforcement must use only its additive /CE inhibit owner"
require_fixed \
    'static bool s_charger_enabled_target;' src/hal/tca8418_hal.c \
    "TCA recovery must retain the charger arbiter's /CE target"
require_fixed \
    'battery_charge_supervisor_service_boot_inhibit_required()' \
    src/app_status_runtime.c \
    "a durable stop latch must own /CE from the first post-reset output"
if rg -q -e 'next_wake|alarm|add_repeating_timer|sleep_ms|busy_wait' \
        src/services/battery_charge_supervisor_logic.c \
        src/services/battery_charge_supervisor_service.c; then
    echo "FAIL: charge supervisor acquired a wake/timer primitive"
    fail=1
fi
unexpected_learning_readers="$(rg -n \
    'battery_learning_service_get_snapshot' src \
    --glob '!src/services/battery_learning_service.c' \
    --glob '!src/services/battery_charge_supervisor_service.c' \
    --glob '!src/services/netmon_diag_service.c' \
    --glob '!src/services/debug_console.c' \
    --glob '!src/app_status_runtime.c' || true)"
if [ -n "$unexpected_learning_readers" ]; then
    echo "FAIL: battery-health output escaped its approved policy/UI consumers"
    printf '%s\n' "$unexpected_learning_readers" | sed 's/^/    /'
    fail=1
fi
require_fixed \
    'battery_learning_service_provision(&provision)' \
    src/services/debug_console.c \
    "battery-health provisioning must remain an explicit CDC-only operation"
unexpected_learning_writers="$(rg -n \
    'battery_learning_service_provision' src \
    --glob '!src/services/battery_learning_service.c' \
    --glob '!src/services/debug_console.c' || true)"
if [ -n "$unexpected_learning_writers" ]; then
    echo "FAIL: battery-learning provisioning escaped the development console"
    printf '%s\n' "$unexpected_learning_writers" | sed 's/^/    /'
    fail=1
fi
if rg -q -e 'next_wake|alarm|add_repeating_timer|sleep_ms|busy_wait' \
        src/services/battery_learning_logic.c \
        src/services/battery_learning_service.c; then
    echo "FAIL: passive battery learning acquired a wake/timer primitive"
    fail=1
fi
require_fixed \
    'composer_octave_tracker_next(&octave, parts.octave)' src/apps/tones/composer.c \
    "composer rests must carry the running octave via the codec's tracker (v6.00 encoder 0x0028b2d6..0x0028b41a), not a fixed 1"
require_fixed \
    '#define NAU_SPK_GAIN_DEFAULT NAU_ANALOG_GAIN_0DB' src/audio/nau88c22_codec.c \
    "the globally calibrated earpiece baseline must remain at 0 dB"
require_fixed \
    '#define NAU_HP_GAIN_DEFAULT NAU_ANALOG_GAIN_0DB' src/audio/nau88c22_codec.c \
    "the headset codec stage must retain its neutral 0 dB baseline"
require_fixed \
    '#define NAU_HP_CALL_GAIN_MAX NAU_HP_GAIN_DEFAULT' src/audio/nau88c22_codec.c \
    "headset calls must keep their bench-confirmed 0 dB Level-10 anchor"
require_fixed \
    '#define AUDIO_HEADSET_LOCAL_GAIN_Q15 1036u' include/audio/audio_levels.h \
    "the measured -30 dB HDC-5 local-audio calibration must remain exact"
require_fixed \
    'sample = headset_local_sample(sample);' src/audio/audio_service.c \
    "headset attenuation must remain on local PCM before modem-downlink mixing"
require_fixed \
    'core1_services_codec_set_call_volume(level)' src/apps/calls_app.c \
    "calls must pass a semantic 1..10 level instead of codec register values"
require_fixed \
    'nau88c22_codec_set_call_volume(s_call_volume_level)' src/services/core1_services.c \
    "runtime codec recovery must restore the selected semantic call volume"
require_fixed \
    'core1_services_codec_reset_call_volume();' src/services/modem_service.c \
    "call teardown must clear recovery-owned volume intent and restore tone baselines"
require_fixed \
    'if (!wanted && !s_bridge_active_wanted && state != MODEM_CALL_ACTIVE) {' \
    src/services/modem_service.c \
    "a call that never formed a bridge must still retire stale volume intent"
require_fixed \
    'static composer_note_event_t events[96];' src/audio/audio_service.c \
    "composer decode scratch stays off the core-1 stack (measured 824 -> 440 B frame)"
require_fixed \
    's_vibra_enabled = marker_vibra;' src/audio/audio_service.c \
    "tone-stream replacement must publish its marker-vibra owner atomically"
require_fixed \
    'audio_arg_with_marker_vibra(ringtone->index, level, vibra != 0u)' \
    src/apps/calls_app.c \
    "incoming ringtone must carry its marker-vibra policy in the start command"
require_fixed \
    'audio_arg_with_marker_vibra(CLOCK_ALARM_TONE_INDEX,' \
    src/apps/clock/alarm.c \
    "alarm must carry its marker-vibra policy in the start command"
require_fixed \
    'audio_arg_with_marker_vibra(ringtone_index,' src/apps/tones/app.c \
    "ringtone preview must carry its marker-vibra policy in the start command"
require_fixed \
    'CORE1_CMD_AUDIO_COMPOSER_PREVIEW_STOP' src/apps/tones/composer.c \
    "Composer preview deadlines must use their owner-scoped stop command"
require_fixed \
    'modem_service_battery_high_load_active()' src/main.c \
    "modem startup/call load must select the fast battery acquisition window"
require_fixed_count_at_least \
    'i2c_set_baudrate(BOARD_I2C_PORT, BOARD_I2C_BAUD_HZ);' src/hal/board.c 2 \
    "both Phase-4 clock transitions must retime shared I2C0"
require_fixed \
    '#define HOOK_PRESS_MV 1900u' src/hal/accessory_hal.c \
    "Rev B2 hook press threshold must preserve the measured 1510/2540 mV margins"
require_fixed \
    '#define HOOK_RELEASE_MV 2200u' src/hal/accessory_hal.c \
    "Rev B2 hook release must retain Schmitt hysteresis"
require_fixed \
    's_consecutive_sample_failures' src/hal/ltc2959_hal.c \
    "successful conversion-start writes must not hide failed sample reads"
require_fixed \
    'ltc2959_acr_delta_nah(raw->acr_raw, LTC2959_ACR_POR_DEFAULT)' \
    src/hal/ltc2959_hal.c \
    "LTC2959 charge delta must survive RP resets in the hardware ACR"
require_fixed \
    's_session_zero_pending' src/hal/ltc2959_hal.c \
    "LTC2959 UVLO recovery must retain an unfulfilled ACR reseed obligation"
require_fixed \
    'gpio_set_input_enabled(AUDIO_PIN_BCLK, true)' src/audio/audio_i2s_hal.c \
    "codec RX PIO WAIT must be able to sample BCLK"
require_fixed \
    'gpio_set_input_enabled(AUDIO_PIN_LRC, true)' src/audio/audio_i2s_hal.c \
    "codec RX PIO WAIT must be able to sample LRCLK"
require_fixed \
    '#define BOARD_DIAG_MODEM_MONITOR_POLL_MS 1000u' src/services/board_diag_service.c \
    "the GP40 diagnostic ADC must remain rate-limited"
if rg -q 'tca8418_hal_input_level' src/services/board_diag_service.c; then
    echo "FAIL: board diagnostics must consume Battery HAL charger snapshots, not poll TCA inputs"
    fail=1
fi

unexpected_usb_owners="$(rg -n \
    'tud_(connected|mounted|suspended|inited|deinit)|stdio_usb_(init|deinit)|usb_hw->' \
    src/main.c src/apps src/hal src/services \
    --glob '!src/services/usb_service.c' || true)"
if [ -n "$unexpected_usb_owners" ]; then
    echo "FAIL: production USB lifecycle escaped usb_service"
    printf '%s\n' "$unexpected_usb_owners" | sed 's/^/    /'
    fail=1
fi
require_fixed \
    'clock_stop(clk_usb);' src/services/usb_service.c \
    "USB-absent policy must stop clk_usb"
require_fixed \
    'USB_USBPHY_DIRECT_TX_PD_BITS' src/services/usb_service.c \
    "USB-absent policy must power down and park the PHY"
require_fixed \
    'log_output_lock();' src/services/usb_service.c \
    "USB lifecycle must serialize stdio-list mutation against cross-core logging"
require_fixed \
    'PICO_STDIO_USB_DEINIT_DELAY_MS=0' CMakeLists.txt \
    "qualified VBUS removal must not stall the product loop for the SDK USB grace delay"
require_fixed \
    'PICO_STDIO_USB_LOW_PRIORITY_IRQ=51' CMakeLists.txt \
    "USB service must own a stable SDK worker IRQ for deterministic liveness kicks"
require_fixed \
    'irq_set_pending(PICO_STDIO_USB_LOW_PRIORITY_IRQ);' src/services/usb_service.c \
    "continuous VBUS must re-pend the SDK TinyUSB worker from the product loop"
if rg -q 'tud_task(_ext)?[[:space:]]*\(' src; then
    echo "FAIL: product code must use only the SDK-owned TinyUSB task worker"
    fail=1
fi
require_fixed \
    '#if SISU_RELEASE_BUILD' src/services/usb_service.c \
    "release USB service must compile to a permanently parked PHY"
require_fixed \
    '#if !SISU_RELEASE_BUILD' src/hal/board_irq_hal.c \
    "release active IRQ policy must omit service VBUS"
require_fixed \
    '#if !SISU_RELEASE_BUILD' src/hal/standby_sleep_hal.c \
    "release powered-standby dormant policy must omit service VBUS"
require_fixed \
    '#if !SISU_RELEASE_BUILD' src/hal/power_sleep_hal.c \
    "release P1.7 policy must omit the GP28 wake slot"
require_fixed \
    'bool release_bootsel_wake =' src/main.c \
    "release USB+Power BOOTSEL recovery must remain reachable through GP7"
require_fixed \
    'release_bootsel_released_ms >= POWER_BUTTON_POWER_ON_HOLD_MS' src/main.c \
    "the release BOOTSEL gate must preserve ordinary power-on holds"
require_fixed \
    'runtime_bootsel_hold_active' src/main.c \
    "the release BOOTSEL gesture must survive the rollbackable soft-off window"

require_fixed \
    'PICO_USE_STACK_GUARDS=1' CMakeLists.txt \
    "both RP2350 cores must retain the hardware MSPLIM stack guard"
require_fixed \
    '$<$<COMPILE_LANGUAGE:C>:-fstack-usage>' CMakeLists.txt \
    "the production build must keep GCC stack-usage records enabled"
require_fixed \
    '$<$<COMPILE_LANGUAGE:C>:-fcallgraph-info=su>' CMakeLists.txt \
    "the production build must keep stack-annotated callgraphs enabled"
require_fixed \
    '${CMAKE_CURRENT_LIST_DIR}/tools/check_stack_budget.py' CMakeLists.txt \
    "the production build must enforce the per-core stack graph budget"
require_fixed \
    '--root core0=main' CMakeLists.txt \
    "the stack budget must retain the core-0 entry point"
require_fixed \
    '--root core1=core1_main' CMakeLists.txt \
    "the stack budget must retain the core-1 entry point"
require_fixed \
    '--irq-reserve 512' CMakeLists.txt \
    "the stack budget must reserve room for nested interrupt frames"
require_fixed \
    '--uncertainty-reserve 512' CMakeLists.txt \
    "the stack budget must reserve room for indirect and library calls"
require_fixed \
    'stack_monitor_core0_init();' src/main.c \
    "core 0 must initialize its runtime stack high-water monitor"
require_fixed \
    'stack_monitor_core0_sample(now);' src/main.c \
    "core 0 must keep sampling its runtime stack high-water monitor"
require_fixed \
    'stack_monitor_core1_init();' src/services/core1_services.c \
    "core 1 must initialize its runtime stack high-water monitor"
require_fixed \
    'stack_monitor_core1_sample(now);' src/services/core1_services.c \
    "core 1 must keep sampling its runtime stack high-water monitor"

watchdog_owner_leaks="$(rg -n \
    '\bwatchdog_(enable|disable|update)\(' src --glob '*.c' \
    | grep -v '^src/services/runtime_watchdog\.c:' || true)"
if [ -n "$watchdog_owner_leaks" ]; then
    echo "FAIL: continuous watchdog ownership escaped runtime_watchdog"
    printf '%s\n' "$watchdog_owner_leaks" | sed 's/^/    /'
    fail=1
fi
require_fixed \
    'runtime_watchdog_feed(RUNTIME_WATCHDOG_PHASE_IDLE);' src/main.c \
    "the main loop must feed only after a complete healthy scheduling turn"
require_fixed \
    'runtime_watchdog_flash_end();' src/storage/nvm_flash_hal.c \
    "flash completion must restore the runtime watchdog lease"

if ! rg -q -U \
    'static void modem_begin_power_on\(uint32_t now_ms\) \{[^}]*!g_modem_vendor\.available' \
    src/services/modem_service.c; then
    echo "FAIL: the rail-on implementation lacks its own backend-availability gate"
    fail=1
fi

telit_pin_leaks="$(rg -n \
    'MODEM_PIN_|hardware/gpio\.h|hal/board\.h|hal/modem_uart_hal\.h' \
    src/services/modem_vendor_telit*.c \
    src/services/modem_vendor_telit_internal.h || true)"
if [ -n "$telit_pin_leaks" ]; then
    echo "FAIL: Telit backend bypasses the neutral modem HAL"
    printf '%s\n' "$telit_pin_leaks" | sed 's/^/    /'
    fail=1
fi

hal_vendor_leaks="$(rg -n \
    'SISU_MODEM_VENDOR_(NONE|TELIT)' include/hal src/hal || true)"
if [ -n "$hal_vendor_leaks" ]; then
    echo "FAIL: HAL code depends on modem vendor identity"
    printf '%s\n' "$hal_vendor_leaks" | sed 's/^/    /'
    fail=1
fi

direct_app_codec_init="$(rg -n \
    '\bnau88c22_codec_init\(' src/main.c src/app*.c src/apps || true)"
if [ -n "$direct_app_codec_init" ]; then
    echo "FAIL: app/runtime codec init bypasses the MCLK + idle-gate contract"
    printf '%s\n' "$direct_app_codec_init" | sed 's/^/    /'
    fail=1
fi

if [ "$fail" -eq 0 ]; then
    echo "Rev B2/Telit static audit passed"
fi
exit "$fail"
