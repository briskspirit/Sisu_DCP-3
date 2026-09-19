#!/usr/bin/env bash
# Host unit tests for the Sisu DCP-3 pure-logic modules.
#
# Each test compiles against the REAL source under test with AddressSanitizer +
# UndefinedBehaviorSanitizer and -Wall -Wextra, so the run doubles as a bug hunt
# (out-of-bounds, overflow, UB in the code under test fail the run, not just
# assertion mismatches). Run from anywhere: tests/run_tests.sh
set -u
cd "$(dirname "$0")/.."

CC="${CC:-cc}"
for tool in "${CC%% *}" python3 rg awk cut grep head sed find sort tr wc mktemp; do
    if ! command -v "$tool" >/dev/null 2>&1; then
        echo "ERROR: host tests require '$tool' (see BUILD.md)" >&2
        exit 2
    fi
done

NONE_FLAGS="-DSISU_HW_REV_B2=1 -DSISU_MODEM_VENDOR_NONE=1 -DSISU_MODEM_BACKEND_ENABLED=0"
TELIT_FLAGS="-DSISU_HW_REV_B2=1 -DSISU_MODEM_VENDOR_TELIT=1 -DSISU_MODEM_BACKEND_ENABLED=1"
MODEM_SERVICE_SOURCES=(
    src/services/modem_service.c
    src/services/operator_name_db.c
    src/services/modem_call_model.c
    src/services/modem_diag_engine.c
    src/services/modem_line_framer.c
    src/services/modem_line_parser.c
    src/services/modem_maintenance.c
    src/services/modem_phonebook_state.c
    src/services/modem_sms_direct.c
    src/services/modem_sms_protocol.c
    src/services/modem_sms_state.c
    src/services/modem_supplementary_state.c
    src/services/sms_deliver_codec.c
    src/services/sms_identity.c
    src/services/sms_picture_codec.c
    src/services/sms_submit_codec.c
    src/services/sms_vvm_filter.c
    src/services/sms_control_filter.c
)
MODEM_TELIT_VENDOR_SOURCES=(
    src/services/modem_vendor_telit.c
    src/services/modem_vendor_telit_call.c
    src/services/modem_vendor_telit_diag.c
    src/services/modem_vendor_telit_maintenance.c
    src/services/modem_vendor_telit_parse.c
    src/services/modem_vendor_telit_operator.c
    src/services/modem_vendor_telit_provision.c
    src/services/modem_vendor_telit_sms.c
    src/services/modem_vendor_telit_tune.c
)
OUT="$(mktemp -d)"
trap 'rm -rf "$OUT"' EXIT

GENERATED_SRC_DIR="src/generated"
GENERATED_INCLUDE_ROOT="include"
using_synthetic_assets=0
required_assets=(
    src/generated/assets_data.c
    src/generated/strings_data.c
    src/generated/tones_data.c
    src/generated/t9_ldb.c
    include/generated/strings_data.h
    include/generated/tones.h
    include/generated/t9_ldb.h
)
assets_missing=0
for asset in "${required_assets[@]}"; do
    if [ ! -f "$asset" ]; then
        assets_missing=1
    fi
done
if [ "${SISU_TEST_FORCE_SYNTHETIC_ASSETS:-0}" = "1" ]; then
    assets_missing=1
fi
if [ "$assets_missing" -ne 0 ]; then
    if [ "${SISU_TEST_REQUIRE_ORIGINAL_ASSETS:-0}" = "1" ]; then
        echo "ERROR: original-firmware test assets are missing." >&2
        echo "       Drop the hash-verified v6.00 image into firmware/ and build once," >&2
        echo "       or unset SISU_TEST_REQUIRE_ORIGINAL_ASSETS for public synthetic fixtures." >&2
        exit 2
    fi
    synthetic_root="$OUT/synthetic-assets"
    if ! python3 tests/generate_synthetic_assets.py "$synthetic_root"; then
        echo "ERROR: failed to generate public synthetic host-test assets" >&2
        exit 2
    fi
    GENERATED_SRC_DIR="$synthetic_root/src/generated"
    GENERATED_INCLUDE_ROOT="$synthetic_root/include"
    using_synthetic_assets=1
    echo "INFO: original assets absent; using temporary synthetic host-test fixtures"
fi

CFLAGS=(
    -std=c11
    -I "$GENERATED_INCLUDE_ROOT"
    -I include
    -I tests
    -I tests/stubs
    -I tools/tune_dynamic_ant
    -Wall
    -Wextra
    -fsanitize=address,undefined
    -fno-sanitize-recover=all
    -g
    -O1
)
if [ "$using_synthetic_assets" -ne 0 ]; then
    CFLAGS+=(-DSISU_SYNTHETIC_ASSETS=1)
fi

# Darwin ld and ELF linkers use different dead-code elimination switches. Probe
# the actual compiler/linker pair instead of assuming that uname identifies it.
printf '%s\n' 'static int unused(void) { return 1; } int main(void) { return 0; }' \
    >"$OUT/link_probe.c"
gc_link_flag=""
for candidate in -Wl,-dead_strip -Wl,--gc-sections; do
    if "$CC" -std=c11 -ffunction-sections -fdata-sections \
            -fsanitize=address,undefined "$OUT/link_probe.c" "$candidate" \
            -o "$OUT/link_probe" >/dev/null 2>&1; then
        gc_link_flag="$candidate"
        break
    fi
done
if [ -z "$gc_link_flag" ]; then
    echo "ERROR: $CC cannot link the sanitizer/dead-code test probe" >&2
    echo "       Install a Clang/GCC toolchain with ASan/UBSan support (see BUILD.md)." >&2
    exit 2
fi
DEAD_CODE_FLAGS=(-ffunction-sections -fdata-sections "$gc_link_flag")

fail=0
pass=0

run_profile_source() {
    profile="$1"; name="$2"; test_source="$3"; shift 3
    case "$profile" in
        none) profile_flags="$NONE_FLAGS" ;;
        telit) profile_flags="$TELIT_FLAGS" ;;
        *) echo "INTERNAL FAIL: unknown test profile '$profile'"; fail=1; return ;;
    esac
    if ! "$CC" "${CFLAGS[@]}" $profile_flags "$test_source" "$@" -o "$OUT/$name" 2>"$OUT/$name.blog"; then
        echo "BUILD FAIL: $name"; sed 's/^/    /' "$OUT/$name.blog"; fail=1; return
    fi
    if "$OUT/$name" >"$OUT/$name.rlog" 2>&1; then
        echo "PASS: $name"; pass=$((pass + 1))
    else
        echo "FAIL: $name"; sed 's/^/    /' "$OUT/$name.rlog"; fail=1
    fi
}

run_profile() {
    profile="$1"; name="$2"; shift 2
    run_profile_source "$profile" "$name" "tests/${name}.c" "$@"
}

run() {
    run_profile none "$@"
}

run_telit() {
    run_profile telit "$@"
}

run_source() {
    name="$1"; test_source="$2"; shift 2
    run_profile_source none "$name" "$test_source" "$@"
}

check_call_model_boundary() {
    name="test_call_model_boundary"
    blog="$OUT/$name.blog"
    if ! deps="$($CC -MM -I include src/services/modem_call_model.c 2>"$blog")"; then
        echo "BUILD FAIL: $name"; sed 's/^/    /' "$blog"; fail=1; return
    fi
    if printf '%s\n' "$deps" | grep -Eq \
        'include/hal/|services/modem_service\.h|services/modem_vendor|services/call_timing_'; then
        echo "FAIL: $name"
        printf '    forbidden model dependency: %s\n' "$deps"
        fail=1
        return
    fi
    echo "PASS: $name"
    pass=$((pass + 1))
}

check_sms_submit_boundary() {
    name="test_sms_submit_boundary"
    blog="$OUT/$name.blog"
    if ! deps="$($CC -MM -I include src/services/sms_submit_codec.c 2>"$blog")"; then
        echo "BUILD FAIL: $name"; sed 's/^/    /' "$blog"; fail=1; return
    fi
    if printf '%s\n' "$deps" | grep -Eq \
        'include/hal/|services/modem_service\.h|services/modem_vendor|storage/'; then
        echo "FAIL: $name"
        printf '    forbidden submit-codec dependency: %s\n' "$deps"
        fail=1
        return
    fi
    if grep -Eq \
        '^[[:space:]]*static[[:space:]].*(sms_binary_(gsm7_code|pack_gsm7|append_hex_byte|append_address|build_submit_pdu)|sms_address_command)[[:space:]]*\(' \
        src/services/modem_service.c; then
        echo "FAIL: $name"
        echo "    private SMS submit codec leaked back into modem_service.c"
        fail=1
        return
    fi
    echo "PASS: $name"
    pass=$((pass + 1))
}

check_sms_module_boundary() {
    name="test_sms_module_boundary"
    leaves="$(find src/services include/services \
        \( -name 'sms_*.c' -o -name 'sms_*.h' \
           -o -name 'modem_sms_*.c' -o -name 'modem_sms_*.h' \) \
        -print | LC_ALL=C sort)"
    if [ -z "$leaves" ]; then
        echo "FAIL: $name (no SMS leaves discovered)"
        fail=1
        return
    fi
    for leaf in $leaves; do
        case "$leaf" in
            *.h) language_flags="-x c" ;;
            *) language_flags="" ;;
        esac
        for profile_flags in "$NONE_FLAGS" "$TELIT_FLAGS"; do
            blog="$OUT/$name.blog"
            if ! deps="$($CC -MM -I include -I tests/stubs $profile_flags \
                $language_flags "$leaf" 2>"$blog")"; then
                echo "BUILD FAIL: $name ($leaf)"
                sed 's/^/    /' "$blog"
                fail=1
                return
            fi
            if printf '%s\n' "$deps" | grep -Eq \
                'include/(apps|hal|storage)/|include/app(_[^/]*)?\.h|services/modem_service\.h|services/modem_vendor[^[:space:]\\]*\.h'; then
                echo "FAIL: $name ($leaf crosses the neutral SMS boundary)"
                printf '    dependency closure: %s\n' "$deps"
                fail=1
                return
            fi
        done
    done
    if grep -Eq \
        's_sms_(mailbox_kind|restore_ok|restore_sim_not_ready|delete_failed|delete_pos)|sms_(binary_start|binary_restore_text_mode|start_status_safe_read|finish_decoded_message)|sms_(submit_format_text_command|pdu_decode|vvm_control_payload_is_recognized)' \
        src/services/modem_service.c; then
        echo "FAIL: $name (operation-local SMS choreography leaked back into modem_service.c)"
        fail=1
        return
    fi
    if grep -Eq \
        'modem_uart_hal|send_command|g_modem_vendor|critical_section|(^|[^[:alnum:]_])s_status([^[:alnum:]_]|$)|services/modem_service\.h' \
        src/services/modem_sms_protocol.c \
        src/services/modem_sms_protocol_internal.h; then
        echo "FAIL: $name (SMS protocol bypasses its typed host-action boundary)"
        fail=1
        return
    fi
    echo "PASS: $name"
    pass=$((pass + 1))
}

check_modem_line_parser_boundary() {
    name="test_modem_line_parser_boundary"
    blog="$OUT/$name.blog"
    if ! deps="$($CC -MM -I include src/services/modem_line_parser.c 2>"$blog")"; then
        echo "BUILD FAIL: $name"; sed 's/^/    /' "$blog"; fail=1; return
    fi
    if printf '%s\n' "$deps" | grep -Eq \
        'include/hal/|services/modem_service\.h|services/modem_vendor|storage/'; then
        echo "FAIL: $name"
        printf '    forbidden line-parser dependency: %s\n' "$deps"
        fail=1
        return
    fi
    if grep -Eq \
        '^[[:space:]]*static[[:space:]].*(parse_cli_fields|copy_quoted_field|sms_status_from_numeric)[[:space:]]*\(|static char s_line_buf|static .*s_line_(len|overflow)|#define MODEM_LINE_MAX' \
        src/services/modem_service.c; then
        echo "FAIL: $name"
        echo "    private line framing/parser state leaked back into modem_service.c"
        fail=1
        return
    fi
    echo "PASS: $name"
    pass=$((pass + 1))
}

check_modem_diag_engine_boundary() {
    name="test_modem_diag_engine_boundary"
    blog="$OUT/$name.blog"
    if ! deps="$($CC -MM -I include src/services/modem_diag_engine.c 2>"$blog")"; then
        echo "BUILD FAIL: $name"; sed 's/^/    /' "$blog"; fail=1; return
    fi
    if printf '%s\n' "$deps" | grep -Eq \
        'include/hal/|services/modem_service\.h|services/modem_vendor|storage/'; then
        echo "FAIL: $name"
        printf '    forbidden diagnostic-engine dependency: %s\n' "$deps"
        fail=1
        return
    fi
    if grep -Eq \
        'static modem_diag_snapshot_t s_diag_(snapshot|shadow)|static modem_diag_group_t s_diag_(selected|active)_group|static uint32_t s_diag_(selected|active)_generation|static uint8_t s_diag_(query_ordinal|query_count|response_lines)|static bool s_diag_(response_invalid|pending|group_active)|static uint32_t s_diag_(command_started_ms|admissions|coalesced|cancelled|completed|command_failures|command_timeouts|malformed_lines|max_command_latency_ms)' \
        src/services/modem_service.c; then
        echo "FAIL: $name"
        echo "    private diagnostic scheduler state leaked back into modem_service.c"
        fail=1
        return
    fi
    echo "PASS: $name"
    pass=$((pass + 1))
}

check_telit_vendor_boundary() {
    name="test_telit_vendor_boundary"
    expected="$(printf '%s\n' "${MODEM_TELIT_VENDOR_SOURCES[@]}" |
        LC_ALL=C sort)"
    actual="$(find src/services -maxdepth 1 -name 'modem_vendor_telit*.c' \
        -print | LC_ALL=C sort)"
    if [ "$actual" != "$expected" ]; then
        echo "FAIL: $name (Telit module set drifted)"
        printf '    expected:\n%s\n    actual:\n%s\n' \
            "$expected" "$actual" | sed 's/^/    /'
        fail=1
        return
    fi

    vendor_sources="$(find src/services -maxdepth 1 \
        -name 'modem_vendor_*.c' -print | LC_ALL=C sort)"
    for source in $vendor_sources; do
        blog="$OUT/$name.blog"
        if ! deps="$($CC -MM -I include -I tests/stubs $TELIT_FLAGS \
            "$source" 2>"$blog")"; then
            echo "BUILD FAIL: $name ($source)"
            sed 's/^/    /' "$blog"
            fail=1
            return
        fi
        if printf '%s\n' "$deps" | grep -Eq \
            'include/(apps|hal|storage)/|include/app(_[^/]*)?\.h|include/services/modem_service\.h'; then
            echo "FAIL: $name ($source reaches app, HAL, storage, or the root modem service)"
            printf '    dependency closure: %s\n' "$deps"
            fail=1
            return
        fi
    done
    cmake_sources="$(grep -Eo \
        'src/services/modem_vendor_telit(_[[:alnum:]_]+)?\.c' \
        CMakeLists.txt | LC_ALL=C sort -u)"
    if [ "$cmake_sources" != "$expected" ]; then
        echo "FAIL: $name (production Telit source membership drifted)"
        printf '    expected:\n%s\n    CMake:\n%s\n' \
            "$expected" "$cmake_sources" | sed 's/^/    /'
        fail=1
        return
    fi
    if grep -Eq 'modem_service_[[:alnum:]_]+' \
        $vendor_sources src/services/modem_vendor_telit_internal.h \
        include/services/modem_vendor.h; then
        echo "FAIL: $name"
        echo "    vendor code must not reference the root modem service"
        fail=1
        return
    fi
    if grep -Eq '(^|[^[:alnum:]_])strtou?l(l)?[[:space:]]*\(' \
        $vendor_sources src/services/modem_vendor_telit_internal.h; then
        echo "FAIL: $name"
        echo "    vendor code must use explicit-width bounded integer parsers"
        fail=1
        return
    fi
    echo "PASS: $name"
    pass=$((pass + 1))
}

check_modem_service_source_membership() {
    name="test_modem_service_source_membership"
    expected="$(printf '%s\n' "${MODEM_SERVICE_SOURCES[@]}" |
        LC_ALL=C sort)"
    actual="$(find src/services -maxdepth 1 \
        \( -name 'modem_*.c' -o -name 'sms_*.c' -o -name 'operator_name_db.c' \) \
        ! -name 'modem_vendor_*.c' -print | LC_ALL=C sort)"
    if [ "$actual" != "$expected" ]; then
        echo "FAIL: $name (shared modem-service source membership drifted)"
        printf '    expected from MODEM_SERVICE_SOURCES:\n%s\n    discovered:\n%s\n' \
            "$expected" "$actual" | sed 's/^/    /'
        fail=1
        return
    fi
    for source in "${MODEM_SERVICE_SOURCES[@]}"; do
        if ! grep -Fq "$source" CMakeLists.txt; then
            echo "FAIL: $name ($source is absent from the production target)"
            fail=1
            return
        fi
    done
    echo "PASS: $name"
    pass=$((pass + 1))
}

check_modem_maintenance_boundary() {
    name="test_modem_maintenance_boundary"
    blog="$OUT/$name.blog"
    if ! deps="$($CC -MM -I include src/services/modem_maintenance.c 2>"$blog")"; then
        echo "BUILD FAIL: $name"; sed 's/^/    /' "$blog"; fail=1; return
    fi
    if printf '%s\n' "$deps" | grep -Eq \
        'include/hal/|services/modem_service\.h|services/modem_vendor|storage/'; then
        echo "FAIL: $name"
        printf '    forbidden maintenance dependency: %s\n' "$deps"
        fail=1
        return
    fi
    if grep -Eq \
        'MAINT_[A-Z0-9_]+|static modem_maintenance_snapshot_t s_maintenance|s_maintenance_(phase|line_seen|line_invalid|cancel_requested|mutated|finishes_error|recovery_checked|original|target|expected|observed)' \
        src/services/modem_service.c; then
        echo "FAIL: $name"
        echo "    private maintenance state leaked back into modem_service.c"
        fail=1
        return
    fi
    echo "PASS: $name"
    pass=$((pass + 1))
}

check_modem_supplementary_boundary() {
    name="test_modem_supplementary_boundary"
    blog="$OUT/$name.blog"
    if ! deps="$($CC -MM -I include src/services/modem_supplementary_state.c 2>"$blog")"; then
        echo "BUILD FAIL: $name"; sed 's/^/    /' "$blog"; fail=1; return
    fi
    if printf '%s\n' "$deps" | grep -Eq \
        'include/hal/|services/modem_service\.h|services/modem_vendor|storage/'; then
        echo "FAIL: $name"
        printf '    forbidden supplementary-state dependency: %s\n' "$deps"
        fail=1
        return
    fi
    if grep -Eq \
        'supplementary_refresh_t|static .*s_(cfu_refresh|voice_mailbox_refresh|message_waiting_refresh|call_forward_(result|row|voice|step|mutated|next_request)|voice_mailbox_(row|candidate)|message_waiting_(row|uncertain|clear_all|category))' \
        src/services/modem_service.c; then
        echo "FAIL: $name"
        echo "    private supplementary state leaked back into modem_service.c"
        fail=1
        return
    fi
    echo "PASS: $name"
    pass=$((pass + 1))
}

check_modem_phonebook_boundary() {
    name="test_modem_phonebook_boundary"
    blog="$OUT/$name.blog"
    if ! deps="$($CC -MM -I include src/services/modem_phonebook_state.c 2>"$blog")"; then
        echo "BUILD FAIL: $name"; sed 's/^/    /' "$blog"; fail=1; return
    fi
    if printf '%s\n' "$deps" | grep -Eq \
        'include/hal/|services/modem_service\.h|services/modem_vendor|storage/'; then
        echo "FAIL: $name"
        printf '    forbidden phonebook-state dependency: %s\n' "$deps"
        fail=1
        return
    fi
    if grep -Eq \
        'static .*s_phonebook_(cache|count|result|result_pending)' \
        src/services/modem_service.c; then
        echo "FAIL: $name"
        echo "    private phonebook state leaked back into modem_service.c"
        fail=1
        return
    fi
    if grep -Eq \
        'AT\+CPB[RSW]|\+CPB[RS]:|MODEM_PHONEBOOK_STORAGE|parse_cpbr_line|phonebook_start_(read|write)' \
        src/services/modem_service.c; then
        echo "FAIL: $name"
        echo "    phonebook parsing, command syntax, or operation steps leaked back into modem_service.c"
        fail=1
        return
    fi
    echo "PASS: $name"
    pass=$((pass + 1))
}

check_modem_link_boundaries() {
    name="test_modem_link_boundaries"
    if grep -Eq '^[[:space:]]*#[[:space:]]*include[[:space:]]*[<"][^>"]*(modem_service|modem_vendor_[[:alnum:]_]+)\.c[>"]' \
        $(find tests -type f \( -name '*.c' -o -name '*.h' \) \
            -print | LC_ALL=C sort); then
        echo "FAIL: $name"
        echo "    modem service/backend tests must link production translation units, not include them"
        fail=1
        return
    fi
    echo "PASS: $name"
    pass=$((pass + 1))
}

check_layering() {
    # Layer contract. Textual matching goes through tests/layering_scan.py
    # (splice/comment-stripped, angle+quote styles, case-folded and
    # ".."-normalized operands) and closure matching through host `cc -MM`
    # with the same normalization, so an include cannot dodge a rule by
    # style, case, or relative path:
    #  (1) service-layer files never include the app layer; the dev-only
    #      debug console's two pinned app_t edges are the only exceptions,
    #      and no OTHER service file may include debug_console.h (that
    #      would import the same app_t transitively).
    #  (2) the app layer's device surface is pinned: apps never include an
    #      audio device header, the keypad HAL, or diag/ (bench tooling
    #      that legitimately bundles device contracts); the direct hal/*
    #      includes are a pinned 4-edge allowlist; and every app file's
    #      TRANSITIVE device closure must equal a pinned per-file set, so a
    #      device dependency growing behind an allowlisted edge (say
    #      battery_hal.h pulling a new hal header) is a deliberate pin
    #      update, not silent drift. main.c is excluded as the hardware
    #      composition root.
    #  (3) closures both ways: no service-side public header reaches an app
    #      header, and no non-device service header re-exports a device
    #      contract. Headers are discovered recursively under include/, and
    #      a preprocess failure is a finding, never a skip -- an unstubbed
    #      SDK include is the one way to hide from -MM.
    #  (4) the ambiguous locations are exact-set pinned (include root,
    #      include/audio, src root): a new file there must be classified --
    #      app layer? device contract? plain service? -- before the guard
    #      passes.
    name="test_layering"
    scan() { python3 tests/layering_scan.py "$@"; }
    audio_devices='audio_i2s_hal|buzzer_hal|modem_i2s_hal|vibra_hal|nau88c22_codec'

    # (4) exact-set pins for the locations the path-based rules cannot
    # classify on their own.
    root_h_allow='include/app.h
include/app_internal.h
include/app_router.h
include/app_runtime.h
include/app_status_runtime.h
include/sisu_build_config.h'
    root_h_actual="$(find include -maxdepth 1 -type f -print | LC_ALL=C sort)"
    root_c_allow='src/app.c
src/app_router.c
src/app_runtime.c
src/app_status_runtime.c
src/main.c'
    root_c_actual="$(find src -maxdepth 1 -type f -print | LC_ALL=C sort)"
    audio_h_allow='audio_bridge.h
audio_i2s_hal.h
audio_levels.h
audio_service.h
buzzer_hal.h
composer_codec.h
modem_i2s_hal.h
nau88c22_codec.h
vibra_hal.h'
    audio_h_actual="$(find include/audio -maxdepth 1 -type f -print \
        | sed 's#^include/audio/##' | LC_ALL=C sort)"
    if [ "$root_h_actual" != "$root_h_allow" ] || [ "$root_c_actual" != "$root_c_allow" ] \
        || [ "$audio_h_actual" != "$audio_h_allow" ]; then
        echo "FAIL: $name (a pinned file set drifted -- classify the new file, then update the pin)"
        printf '    include root:\n%s\n    src root:\n%s\n    include/audio:\n%s\n' \
            "$root_h_actual" "$root_c_actual" "$audio_h_actual" | sed 's/^/    /'
        fail=1; return
    fi

    # One textual pass over every first-party source and header (tests and
    # generated outputs excluded).
    production_files="$(find src include -type f \( -name '*.c' -o -name '*.h' \) \
        | LC_ALL=C sort)"
    if ! scan classify $production_files >"$OUT/layering_classes.tsv"; then
        echo "FAIL: $name (a production file is in an unregistered layer)"
        fail=1; return
    fi
    incs="$OUT/layering_includes.tsv"
    if ! scan includes $(find src include \( -name '*.c' -o -name '*.h' \) \
        ! -path 'src/generated/*' ! -path 'include/generated/*' | LC_ALL=C sort) >"$incs"; then
        echo "FAIL: $name (include scanner failed)"; fail=1; return
    fi

    # Computed operands, #include_next, and upward/absolute paths defeat
    # textual matching entirely -- banned outright (none exist, none needed).
    bad="$(awk -F'\t' '$2 == "x" || $3 ~ /^\.\./ || $3 ~ /^\// {print $1": "$3}' "$incs")"
    if [ -n "$bad" ]; then
        echo "FAIL: $name (computed / #include_next / upward-relative / absolute include)"
        printf '%s\n' "$bad" | sed 's/^/    /'
        fail=1; return
    fi

    svc_rows="$OUT/layering_svc_rows.tsv"
    app_rows="$OUT/layering_app_rows.tsv"
    awk -F'\t' '$1 ~ /^(src|include)\/(services|audio|storage|ui|hal|diag)\//' "$incs" >"$svc_rows"
    awk -F'\t' '$1 ~ /^(src\/apps\/|include\/apps\/|src\/app(_runtime|_router|_status_runtime)?\.c$|include\/app(_internal|_router|_runtime|_status_runtime)?\.h$)/' "$incs" >"$app_rows"

    # (1) service->app direct includes: the pinned console exception only.
    # app.h itself imports app modules, so these are honest exceptions
    # rather than a claim that the dependency is type-only.
    service_app_allow='include/services/debug_console.h:app.h
src/services/debug_console.c:app_internal.h'
    service_app_actual="$(awk -F'\t' '$3 ~ /^apps\// || $3 ~ /^app(_internal|_router|_runtime|_status_runtime)?\.h$/ {print $1":"$3}' "$svc_rows" | LC_ALL=C sort)"
    if [ "$service_app_actual" != "$service_app_allow" ]; then
        echo "FAIL: $name (service->app include set drifted from the pinned console exception)"
        printf '    expected:\n%s\n    actual:\n%s\n' "$service_app_allow" "$service_app_actual" | sed 's/^/    /'
        fail=1; return
    fi
    console_importers="$(awk -F'\t' '$3 == "services/debug_console.h" && $1 != "src/services/debug_console.c" {print $1}' "$svc_rows")"
    if [ -n "$console_importers" ]; then
        echo "FAIL: $name (service file imports app_t via services/debug_console.h)"
        printf '%s\n' "$console_importers" | sed 's/^/    /'
        fail=1; return
    fi

    # (2) app-layer textual rules: device bans + the pinned hal allowlist.
    app_dev_bad="$(awk -F'\t' '{print $1": "$3}' "$app_rows" \
        | grep -E ": (diag/|audio/(${audio_devices})\.h\$)")"
    if [ -n "$app_dev_bad" ]; then
        echo "FAIL: $name (app-layer file includes an audio device / diag header)"
        printf '%s\n' "$app_dev_bad" | sed 's/^/    /'
        fail=1; return
    fi
    # The remaining direct hal/ edges are deliberate, not drift:
    #  - rtc_alarm_hal: the RTC is the clock/alarm feature's own device (its
    #    HH:MM alarm, snooze, datetime), exactly as the keypad is input's; a
    #    pass-through "clock service" would add a layer with no logic in it.
    #    clock/app owns datetime/config synchronization, clock/editor owns
    #    user-entered datetime reads/writes, and clock/alarm owns event/snooze
    #    runtime; power_app and app_status_runtime read the alarm-fire/snooze
    #    evidence the boot path and standby need.
    # Battery estimate/warning evidence reaches app_status_runtime through
    # board_diag_service. Its separate raw-collapse floor lives with the pure
    # app policy in battery_status_logic, so no battery HAL edge is needed.
    hal_allow='src/app_status_runtime.c:hal/rtc_alarm_hal.h
src/apps/clock/alarm.c:hal/rtc_alarm_hal.h
src/apps/clock/app.c:hal/rtc_alarm_hal.h
src/apps/clock/editor.c:hal/rtc_alarm_hal.h
src/apps/power_app.c:hal/rtc_alarm_hal.h'
    hal_actual="$(awk -F'\t' '$3 ~ /^hal\// {print $1":"$3}' "$app_rows" | LC_ALL=C sort)"
    if [ "$hal_actual" != "$hal_allow" ]; then
        echo "FAIL: $name (app->HAL include set drifted from the pinned allowlist)"
        printf '    expected:\n%s\n    actual:\n%s\n' "$hal_allow" "$hal_actual" | sed 's/^/    /'
        fail=1; return
    fi

    # (3) closure passes. Check the union of the production Telit and the
    # modem-free diagnostic profiles: a conditional include in either profile
    # must not hide a dependency. A nonzero return means the file does not
    # preprocess under one of the supported profiles, which every caller treats
    # as a failure, never a skip.
    norm_closure_profile() {
        local mm
        local profile_flags="$2"
        mm="$("$CC" -std=c11 -MM -I "$GENERATED_INCLUDE_ROOT" -I include \
            -I tests/stubs $profile_flags -x c "$1" 2>/dev/null)" || return 1
        printf '%s\n' "$mm" | scan normdeps
    }
    norm_closure() {
        local none telit
        none="$(norm_closure_profile "$1" "$NONE_FLAGS")" || return 1
        telit="$(norm_closure_profile "$1" "$TELIT_FLAGS")" || return 1
        printf '%s\n%s\n' "$none" "$telit" | sed '/^$/d' | LC_ALL=C sort -u
    }
    app_hdr_re='^include/app(_internal|_router|_runtime|_status_runtime)?\.h$|^include/apps/'
    device_closure_re="^include/hal/|^include/diag/|^include/audio/(${audio_devices})\.h$"

    fwd_leaks=""; rev_leaks=""
    for hdr in $(find include -name '*.h' \
        ! -path 'include/apps/*' ! -path 'include/generated/*' \
        ! -path include/app.h ! -path include/app_internal.h \
        ! -path include/app_router.h ! -path include/app_runtime.h \
        ! -path include/app_status_runtime.h | LC_ALL=C sort); do
        if ! deps="$(norm_closure "$hdr")"; then
            fwd_leaks="$fwd_leaks
$hdr: (failed to preprocess on host -- keep public headers host-preprocessable; stub any SDK surface)"
            continue
        fi
        if [ "$hdr" != "include/services/debug_console.h" ] \
            && printf '%s\n' "$deps" | grep -qE "$app_hdr_re"; then
            fwd_leaks="$fwd_leaks
$hdr"
        fi
        if ! printf '%s\n' "$hdr" | grep -qE "^include/(hal|diag)/|^include/audio/(${audio_devices})\.h$" \
            && printf '%s\n' "$deps" | grep -qE "^include/hal/|^include/audio/(${audio_devices})\.h$"; then
            rev_leaks="$rev_leaks
$hdr"
        fi
    done
    if [ -n "$fwd_leaks" ]; then
        echo "FAIL: $name (service-side header transitively reaches an app header)"
        printf '%s\n' "$fwd_leaks" | sed '/^$/d; s/^/    /'
        fail=1; return
    fi
    if [ -n "$rev_leaks" ]; then
        echo "FAIL: $name (non-device service header re-exports a device contract to its consumers)"
        printf '%s\n' "$rev_leaks" | sed '/^$/d; s/^/    /'
        fail=1; return
    fi

    # App closure pass: per-file transitive device surface, exact-pinned.
    app_closure_allow='src/app_status_runtime.c: include/hal/rtc_alarm_hal.h
src/apps/clock/alarm.c: include/hal/rtc_alarm_hal.h
src/apps/clock/app.c: include/hal/rtc_alarm_hal.h
src/apps/clock/editor.c: include/hal/rtc_alarm_hal.h
src/apps/power_app.c: include/hal/rtc_alarm_hal.h'
    app_preproc_fail=""; app_closure_actual=""
    for f in src/app.c src/app_runtime.c src/app_router.c src/app_status_runtime.c \
        $(find src/apps -name '*.c' -print | LC_ALL=C sort) \
        include/app.h include/app_internal.h include/app_router.h \
        include/app_runtime.h include/app_status_runtime.h include/apps/*.h; do
        if ! deps="$(norm_closure "$f")"; then
            app_preproc_fail="$app_preproc_fail
$f: (failed to preprocess on host -- the app layer must stay SDK-free)"
            continue
        fi
        devs="$(printf '%s\n' "$deps" | grep -E "$device_closure_re" | LC_ALL=C sort -u | tr '\n' ' ')"
        if [ -n "$devs" ]; then
            app_closure_actual="$app_closure_actual
$f: ${devs% }"
        fi
    done
    if [ -n "$app_preproc_fail" ]; then
        echo "FAIL: $name (app-layer file does not preprocess on the host)"
        printf '%s\n' "$app_preproc_fail" | sed '/^$/d; s/^/    /'
        fail=1; return
    fi
    app_closure_actual="$(printf '%s\n' "$app_closure_actual" | sed '/^$/d' | LC_ALL=C sort)"
    if [ "$app_closure_actual" != "$app_closure_allow" ]; then
        echo "FAIL: $name (app-layer transitive device closure drifted from the pinned per-file sets)"
        printf '    expected:\n%s\n    actual:\n%s\n' "$app_closure_allow" "$app_closure_actual" | sed 's/^/    /'
        fail=1; return
    fi
    echo "PASS: $name"
    pass=$((pass + 1))
}

check_layering_scan() {
    name="test_layering_scan"
    rlog="$OUT/$name.rlog"
    if python3 tests/test_layering_scan.py >"$rlog" 2>&1; then
        echo "PASS: $name"
        pass=$((pass + 1))
    else
        echo "FAIL: $name"
        sed 's/^/    /' "$rlog"
        fail=1
    fi
}

check_assetgen() {
    name="test_assetgen"
    rlog="$OUT/$name.rlog"
    if python3 tests/test_assetgen.py >"$rlog" 2>&1; then
        echo "PASS: $name"
        pass=$((pass + 1))
    else
        echo "FAIL: $name"
        sed 's/^/    /' "$rlog"
        fail=1
    fi
}

check_stack_budget() {
    name="test_stack_budget"
    rlog="$OUT/$name.rlog"
    if python3 tests/test_stack_budget.py >"$rlog" 2>&1; then
        echo "PASS: $name"
        pass=$((pass + 1))
    else
        echo "FAIL: $name"
        sed 's/^/    /' "$rlog"
        fail=1
    fi
}

check_revb2_static_audit() {
    name="test_revb2_static_audit"
    rlog="$OUT/$name.rlog"
    if bash tests/test_revb2_static_audit.sh >"$rlog" 2>&1; then
        echo "PASS: $name"
        pass=$((pass + 1))
    else
        echo "FAIL: $name"
        sed 's/^/    /' "$rlog"
        fail=1
    fi
}

# test-name                  sources under test  (modules with hardware deps are
#                            stubbed in-test, so only pure sources are linked here)
run test_text_fit            src/ui/assets.c "$GENERATED_SRC_DIR/assets_data.c"
run test_ui_wrap             src/ui/ui.c src/ui/text_layout.c src/ui/assets.c "$GENERATED_SRC_DIR/assets_data.c" src/ui/framebuffer.c src/services/strings.c "$GENERATED_SRC_DIR/strings_data.c"
run test_sms_picture_codec   src/services/sms_picture_codec.c
run test_sms_deliver_codec   src/services/sms_deliver_codec.c src/services/sms_picture_codec.c
run test_modem_sms_direct    src/services/modem_sms_direct.c src/services/sms_deliver_codec.c src/services/sms_picture_codec.c src/services/sms_control_filter.c src/services/sms_vvm_filter.c
run test_sms_control_filter  src/services/sms_control_filter.c src/services/sms_vvm_filter.c src/services/sms_picture_codec.c src/services/sms_deliver_codec.c
run test_sms_vvm_filter      src/services/sms_vvm_filter.c
run test_sms_types
run test_audio_tonedecode    "$GENERATED_SRC_DIR/tones_data.c" src/audio/composer_codec.c src/audio/audio_levels.c
run test_composer_codec      src/audio/composer_codec.c
run test_tone_composer_app   src/apps/tones/app.c src/apps/tones/composer.c src/audio/composer_codec.c src/audio/audio_levels.c src/ui/menu_visible.c src/ui/assets.c "$GENERATED_SRC_DIR/assets_data.c" src/ui/framebuffer.c "${DEAD_CODE_FLAGS[@]}"
run test_rv8803_regs         src/hal/rv8803_regs.c
run test_rv8803_hal          src/hal/rv8803_hal.c src/hal/rv8803_regs.c
run test_rtc_alarm_hal       src/hal/rtc_alarm_hal.c src/hal/rv8803_regs.c
run test_power_sleep         src/services/power_sleep_logic.c src/hal/power_sleep_wake_logic.c
run_source test_runtime_watchdog_cold tests/test_runtime_watchdog.c src/services/runtime_watchdog.c -DTEST_WATCHDOG_BOOT_CASE=0
run_source test_runtime_watchdog_prior tests/test_runtime_watchdog.c src/services/runtime_watchdog.c -DTEST_WATCHDOG_BOOT_CASE=1
run_source test_runtime_watchdog_malformed tests/test_runtime_watchdog.c src/services/runtime_watchdog.c -DTEST_WATCHDOG_BOOT_CASE=2
run test_standby_sleep_logic src/services/standby_sleep_logic.c
run test_critical_section_stub tests/test_critical_section_stub_peer.c
run test_stack_monitor_logic src/services/stack_monitor_logic.c
run test_audio_levels        src/audio/audio_levels.c
run test_game_common         src/apps/game_common.c src/audio/audio_levels.c
run test_framebuffer         src/ui/framebuffer.c
run test_menu_visible        src/ui/menu_visible.c
run test_status_chrome       src/ui/status_chrome.c src/ui/framebuffer.c src/ui/assets.c "$GENERATED_SRC_DIR/assets_data.c"
run test_signal_bars         src/ui/signal_bars.c
run test_storage_journal     src/storage/storage_journal.c
run test_nvm_flash_hal       src/storage/nvm_flash_hal.c -DTEST_NVM_IRQ_ORDER
run test_storage_lfs         src/storage/storage_lfs.c third_party/littlefs/lfs.c third_party/littlefs/lfs_util.c -I third_party/littlefs -DLFS_NO_MALLOC -DLFS_NO_DEBUG -DLFS_NO_WARN -DLFS_NO_ERROR
run test_storage_lfs_growth  src/storage/storage_lfs.c third_party/littlefs/lfs.c third_party/littlefs/lfs_util.c -I third_party/littlefs -DLFS_NO_MALLOC -DLFS_NO_DEBUG -DLFS_NO_WARN -DLFS_NO_ERROR
run test_storage_objects     src/storage/storage_lfs.c third_party/littlefs/lfs.c third_party/littlefs/lfs_util.c -I third_party/littlefs -DLFS_NO_MALLOC -DLFS_NO_DEBUG -DLFS_NO_WARN -DLFS_NO_ERROR
run test_storage_powercut    src/diag/storage_powercut_test.c src/storage/storage_lfs.c third_party/littlefs/lfs.c third_party/littlefs/lfs_util.c -I src/storage -I third_party/littlefs -DLFS_NO_MALLOC -DLFS_NO_DEBUG -DLFS_NO_WARN -DLFS_NO_ERROR
run test_storage_partitions  src/storage/storage_partitions.c src/storage/storage_lfs.c third_party/littlefs/lfs.c third_party/littlefs/lfs_util.c -I third_party/littlefs -DLFS_NO_MALLOC -DLFS_NO_DEBUG -DLFS_NO_WARN -DLFS_NO_ERROR
run test_store_service       src/storage/store_calls.c src/storage/store_battery_learning.c src/storage/store_battery_charge_supervisor.c src/storage/store_service.c src/storage/store_divert.c src/storage/store_pictures.c src/storage/store_settings.c src/storage/store_t9.c src/storage/store_tones.c src/storage/store_warranty.c src/storage/storage_lfs.c third_party/littlefs/lfs.c third_party/littlefs/lfs_util.c src/services/battery_learning_logic.c src/services/battery_charge_supervisor_logic.c src/services/battery_charge_logic.c src/services/sms_picture_codec.c -I src -I third_party/littlefs -DLFS_NO_MALLOC -DLFS_NO_DEBUG -DLFS_NO_WARN -DLFS_NO_ERROR
run test_t9_service          src/services/t9_service.c "$GENERATED_SRC_DIR/t9_ldb.c"
run test_phone_match         src/services/phone_match.c
run test_clock_alarm_logic   src/apps/clock_alarm_logic.c
run test_clock_alarm_app     src/apps/clock/app.c src/apps/clock/alarm.c src/apps/clock_alarm_logic.c src/audio/audio_levels.c "${DEAD_CODE_FLAGS[@]}"
run test_clock_date_logic    src/apps/clock_date_logic.c
run test_clock_editor_app    src/apps/clock/app.c src/apps/clock/editor.c src/apps/clock_date_logic.c src/audio/audio_levels.c src/services/key_utils.c "${DEAD_CODE_FLAGS[@]}"
run test_call_forward_mmi    src/services/call_forward_mmi.c
run test_call_divert_app     src/apps/call_divert_app.c src/services/call_forward_mmi.c "${DEAD_CODE_FLAGS[@]}"
run test_call_options_logic  src/apps/call_options_logic.c
run test_call_newcall_preserve src/apps/calls_app.c "${DEAD_CODE_FLAGS[@]}"  # New-call snapshot/restore; whole app compiled, unreachable code stripped
run test_call_waiting_handoff src/apps/calls_app.c "${DEAD_CODE_FLAGS[@]}"  # generation-qualified remote-release handoff
run test_phonebook_speed_call src/apps/phonebook_app.c src/services/feature_gates.c src/ui/menu_visible.c src/ui/assets.c "$GENERATED_SRC_DIR/assets_data.c" src/ui/framebuffer.c "${DEAD_CODE_FLAGS[@]}"
run test_profiles_app       src/apps/profiles_app.c "${DEAD_CODE_FLAGS[@]}"
run test_settings_restore   src/apps/settings_app.c "${DEAD_CODE_FLAGS[@]}"
run test_power_app          src/apps/power_app.c "${DEAD_CODE_FLAGS[@]}"
run test_powerup_app        src/apps/powerup_app.c src/audio/audio_levels.c "${DEAD_CODE_FLAGS[@]}"
run test_snake_app          src/apps/snake_app.c "${DEAD_CODE_FLAGS[@]}"
run test_display_wait        src/apps/dialogs_app.c src/ui/text_layout.c src/ui/assets.c src/ui/framebuffer.c "$GENERATED_SRC_DIR/assets_data.c" "${DEAD_CODE_FLAGS[@]}"
run test_calculator_app      src/apps/calculator_app.c src/services/key_utils.c "${DEAD_CODE_FLAGS[@]}"
run test_net_monitor_logic   src/apps/net_monitor/logic.c
run_telit test_net_monitor_render \
    src/apps/net_monitor/render.c \
    src/apps/net_monitor/render_helpers.c \
    src/apps/net_monitor/render_radio.c \
    src/apps/net_monitor/render_modem.c \
    src/apps/net_monitor/render_telephony.c \
    src/apps/net_monitor/render_local.c \
    src/apps/net_monitor/render_control.c \
    src/apps/net_monitor/logic.c \
    src/ui/assets.c "$GENERATED_SRC_DIR/assets_data.c"
run test_netmon_control_service src/services/netmon_control_service.c src/audio/audio_levels.c
run test_sim_presence_logic  src/apps/sim_presence_logic.c
run test_battery_gauge       src/hal/battery_gauge_logic.c
run test_battery_learning_logic src/services/battery_learning_logic.c
run test_battery_learning_service src/services/battery_learning_service.c src/services/battery_learning_logic.c
run test_board_safe_gpio     src/hal/board_safe_gpio.c
run test_service_vbus_logic  src/hal/service_vbus_logic.c
run test_charger_input_logic src/hal/charger_input_logic.c
run test_battery_charge_logic src/services/battery_charge_logic.c
run test_battery_charge_supervisor_logic src/services/battery_charge_supervisor_logic.c src/services/battery_charge_logic.c src/services/battery_learning_logic.c
run test_battery_charge_supervisor_service src/services/battery_charge_supervisor_service.c src/services/battery_charge_supervisor_logic.c src/services/battery_charge_logic.c src/services/battery_learning_logic.c
run test_charger_control_service src/services/charger_control_service.c
run test_bq25171_stat        src/hal/bq25171_stat.c
run test_battery_poll_logic   src/hal/battery_poll_logic.c
run test_battery_status_logic src/apps/battery_status_logic.c
run test_battery_runtime_logic src/hal/battery_runtime_logic.c
run test_board_diag_power_gate src/services/board_diag_service.c src/services/charger_control_service.c "${DEAD_CODE_FLAGS[@]}"
run test_ltc2959_regs        src/hal/ltc2959_regs.c
run test_ltc2959_hal         src/hal/ltc2959_hal.c src/hal/ltc2959_regs.c
run test_tca8418_hal         src/hal/tca8418_hal.c
run test_keypad              src/hal/keypad.c src/services/event_queue.c
run test_modem_power_monitor src/hal/modem_power_monitor_logic.c
run test_modem_uart_flow_logic
run test_telit_diag_logic    src/diag/telit_diag_logic.c
run_source test_telit_tuner_logic tools/tune_dynamic_ant/test_tuner_logic.c tools/tune_dynamic_ant/tuner_logic.c
run test_shared_irq_logic    src/services/shared_irq_logic.c
run test_shared_3v8_service  src/services/shared_3v8_service.c
check_revb2_static_audit
run test_backlight_calibration src/services/backlight_calibration.c
run test_lcd_calibration     src/services/lcd_calibration.c
check_call_model_boundary    # transitive include graph must stay HAL/vendor/service-free
check_sms_submit_boundary    # pure PDU codec must not depend on the root service
check_sms_module_boundary    # every SMS leaf stays root/vendor/app/HAL/storage independent
check_modem_line_parser_boundary # typed parser must not depend on root/vendor/HAL
check_modem_diag_engine_boundary # scheduler state stays vendor/root/HAL independent
check_telit_vendor_boundary # split vendor modules stay HAL/app/storage-free and production-linked
check_modem_maintenance_boundary # state machine stays storage/vendor/root independent
check_modem_supplementary_boundary # caches/collectors stay transport/vendor independent
check_modem_phonebook_boundary # complete phonebook owner stays transport/vendor independent
check_modem_service_source_membership # every core modem/SMS TU is shared by both service profiles
check_modem_link_boundaries # service/backend remain separately linked translation units
check_layering_scan          # synthetic scanner cases: splices/comments/styles/path normalization
check_layering               # both-direction include closures + pinned sets; see the contract comment in the function
run test_call_model_events   src/services/modem_call_model.c
run test_strings             src/services/strings.c "$GENERATED_SRC_DIR/strings_data.c"
run test_message_waiting_ui  src/services/strings.c "$GENERATED_SRC_DIR/strings_data.c" "${DEAD_CODE_FLAGS[@]}"
run test_messages_app src/apps/messages/app.c src/apps/messages/composer.c src/apps/messages/picture.c src/services/key_utils.c src/ui/menu_visible.c src/ui/text_layout.c "${DEAD_CODE_FLAGS[@]}"
if [ "$using_synthetic_assets" -eq 0 ]; then
    run test_picture_ui src/apps/messages/picture.c src/apps/dialogs_app.c src/ui/ui.c src/ui/text_layout.c src/ui/assets.c src/ui/framebuffer.c src/services/strings.c "$GENERATED_SRC_DIR/assets_data.c" "$GENERATED_SRC_DIR/strings_data.c" "${DEAD_CODE_FLAGS[@]}"
fi
check_assetgen               # asset-generation pipeline pure logic (python, no firmware needed)
check_stack_budget           # GCC callgraph parser + core budget policy
run test_small_utils         # includes the pure .c files directly (single TU, stubbed HALs)
run test_modem_pdu           src/services/sms_submit_codec.c
run test_modem_diag_engine   src/services/modem_diag_engine.c
run test_modem_maintenance   src/services/modem_maintenance.c
run test_modem_supplementary_state src/services/modem_supplementary_state.c
run test_modem_phonebook_state src/services/modem_phonebook_state.c -I src
run test_modem_phonebook_protocol src/services/modem_phonebook_state.c -I src
run test_modem_sms_state src/services/modem_sms_state.c \
    src/services/sms_identity.c src/services/sms_submit_codec.c -I src
run test_modem_sms_protocol src/services/modem_sms_protocol.c \
    src/services/modem_sms_state.c src/services/modem_line_parser.c \
    src/services/sms_identity.c src/services/sms_picture_codec.c \
    src/services/sms_submit_codec.c src/services/sms_vvm_filter.c -I src
run test_modem_none_service -DSISU_MODEM_SERVICE_TEST=1 "${MODEM_SERVICE_SOURCES[@]}" src/services/modem_vendor_none.c
run test_modem_service_parsers src/services/modem_line_framer.c src/services/modem_line_parser.c
run test_operator_name_db
run_telit test_telit_operator_name src/services/modem_vendor_telit_operator.c
run_telit test_modem_vendor_telit "${MODEM_TELIT_VENDOR_SOURCES[@]}" \
    src/services/sms_deliver_codec.c src/services/modem_sms_direct.c src/services/sms_picture_codec.c \
    src/services/sms_control_filter.c src/services/sms_vvm_filter.c
run_telit test_modem_telit_service -DSISU_MODEM_SERVICE_TEST=1 \
    "${MODEM_SERVICE_SOURCES[@]}" "${MODEM_TELIT_VENDOR_SOURCES[@]}"
run test_call_model_golden   src/services/modem_call_model.c  # public-API golden traces
run test_call_model_sched    src/services/modem_call_model.c
run test_modem_call_model_clcc src/services/modem_call_model.c
run test_modem_call_model_project  src/services/modem_call_model.c
run test_modem_harness       tests/harness/modem_harness.c src/services/modem_line_framer.c -I tests  # black-box harness self-test
run test_modem_call_model    # includes modem_call_model.c directly (pure model, single TU)
run test_call_model_second_result_reset      # second_call_result reset lifecycle (§9.5/§16.9); includes modem_call_model.c directly
run test_call_model_wait_reject_preid        # pre-ID WAIT_REJECT SUCCEEDED handling (§16.5); includes modem_call_model.c directly
run test_call_model_txn_abandon              # a bound_id-only abandon must release the target op's leg (§16.5/§16.11); includes modem_call_model.c directly
run test_call_model_terminal_cause_ownership # a foreign leg eviction must not steal an unbound 2nd-MO's terminal cause (§16.3/§16.4); includes modem_call_model.c directly
run test_call_model_dispatch_guard           # the WAIT_REJECT pre-ID dispatch-guard must be total (§16.5); includes modem_call_model.c directly
run test_call_model_txn_matrix               # transaction/cause/tombstone/pre-ID public-API matrix

echo "----"
if [ "$fail" -eq 0 ]; then
    echo "ALL TESTS PASSED ($pass)"
else
    echo "TESTS FAILED"
fi
exit $fail
