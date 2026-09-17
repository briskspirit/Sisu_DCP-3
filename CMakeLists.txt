cmake_minimum_required(VERSION 3.13)

# Custom RP2354B board definition. Must be set before pico_sdk_init(), which
# runs cmake/generic_board.cmake to locate <PICO_BOARD>.h.
list(APPEND PICO_BOARD_HEADER_DIRS ${CMAKE_CURRENT_LIST_DIR}/boards)
set(PICO_BOARD sisu_sse_revb2 CACHE STRING "Target board" FORCE)
# Prefer an already-vendored SDK checkout. FetchContent records absolute paths in
# .pico-sdk/pico_sdk-subbuild, so a populated tree that is later moved/copied fails
# to configure with "CMakeCache.txt directory is different". Pointing PICO_SDK_PATH
# straight at the source bypasses FetchContent and is relocation-safe; the git fetch
# below stays as the fallback for a clean checkout where .pico-sdk does not yet exist.
if(NOT PICO_SDK_PATH AND NOT DEFINED ENV{PICO_SDK_PATH}
   AND EXISTS "${CMAKE_CURRENT_LIST_DIR}/.pico-sdk/pico_sdk-src/pico_sdk_init.cmake")
    set(PICO_SDK_PATH "${CMAKE_CURRENT_LIST_DIR}/.pico-sdk/pico_sdk-src")
    message(STATUS "Using vendored pico-sdk at ${PICO_SDK_PATH}")
endif()

set(SISU_PICO_SDK_QUALIFIED_COMMIT
    "a1438dff1d38bd9c65dbd693f0e5db4b9ae91779")
set(SISU_ALLOW_UNQUALIFIED_PICO_SDK OFF CACHE BOOL
    "Allow an SDK other than the clean, qualified pico-sdk commit (development only)")
set(PICO_SDK_FETCH_FROM_GIT ON CACHE BOOL "Fetch pico-sdk when PICO_SDK_PATH is not set")
# FORCE also upgrades an existing build cache that still names the mutable 2.2.0
# tag. Alternate SDKs remain possible only through the explicit unqualified-SDK
# switch, so an ordinary configure cannot silently change the toolchain.
set(PICO_SDK_FETCH_FROM_GIT_TAG "${SISU_PICO_SDK_QUALIFIED_COMMIT}" CACHE STRING
    "Exact pico-sdk git revision" FORCE)
set(PICO_SDK_FETCH_FROM_GIT_PATH "${CMAKE_CURRENT_LIST_DIR}/.pico-sdk" CACHE PATH "Local pico-sdk fetch cache")

include(cmake/pico_sdk_import.cmake)

project(sisu_dcp3_revb2 C CXX ASM)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)

pico_sdk_init()

set(SISU_FIRMWARE_TARGET sisu_dcp3_revb2_telit)
option(RELEASE
    "Build the release firmware without USB CDC or the service console" OFF)
if(RELEASE)
    set(SISU_RELEASE_BUILD_VALUE 1)
    set(SISU_DEBUG_CONSOLE_SOURCE "")
else()
    set(SISU_RELEASE_BUILD_VALUE 0)
    set(SISU_DEBUG_CONSOLE_SOURCE src/services/debug_console.c)
endif()

# Build-time asset generation (see firmware/README.md + tools/assetgen).
# The generated files are NEVER committed. The DEFAULT selection generates
# into src/generated + include/generated (kept fresh from the firmware when
# present; usable as-is on later builds without it). Any other selection
# requires the firmware and is emitted privately into this build directory,
# so concurrent build directories never fight over shared files.
find_program(SISU_PYTHON3 python3)
if(NOT SISU_PYTHON3)
    message(FATAL_ERROR "python3 is required for asset generation (tools/assetgen)")
endif()
set(SISU_ASSET_LANGS "shipped" CACHE STRING
    "UI language set to compile: shipped, all, or a comma list of pack stems (English always included)")
set(SISU_T9_LANGS "ENGL,GER,FREN" CACHE STRING
    "T9 dictionaries to compile: comma list of tags (English always first)")
if(SISU_ASSET_LANGS STREQUAL "shipped" AND SISU_T9_LANGS STREQUAL "ENGL,GER,FREN")
    set(SISU_ASSETS_EMIT_DIR "")
    set(SISU_GENERATED_SRC_DIR ${CMAKE_CURRENT_SOURCE_DIR}/src/generated)
    set(SISU_GENERATED_INC_DIR ${CMAKE_CURRENT_SOURCE_DIR}/include)
else()
    set(SISU_ASSETS_EMIT_DIR ${CMAKE_BINARY_DIR}/generated_assets)
    set(SISU_GENERATED_SRC_DIR ${SISU_ASSETS_EMIT_DIR}/src)
    set(SISU_GENERATED_INC_DIR ${SISU_ASSETS_EMIT_DIR}/include)
endif()
set(SISU_GENERATED_SOURCES
    ${SISU_GENERATED_SRC_DIR}/assets_data.c
    ${SISU_GENERATED_SRC_DIR}/tones_data.c
    ${SISU_GENERATED_SRC_DIR}/t9_ldb.c
    ${SISU_GENERATED_SRC_DIR}/strings_data.c)
# Always GENERATED: the files are never committed, so on a fresh checkout
# they do not exist at configure time in EITHER mode -- the sisu_assets
# target produces them before the first compile (or fails with instructions).
set_source_files_properties(${SISU_GENERATED_SOURCES} PROPERTIES GENERATED TRUE)

add_executable(${SISU_FIRMWARE_TARGET}
    src/app.c
    src/app_router.c
    src/app_runtime.c
    src/app_status_runtime.c
    src/audio/audio_bridge.c
    src/audio/audio_i2s_hal.c
    src/audio/audio_levels.c
    src/audio/audio_service.c
    src/audio/composer_codec.c
    src/audio/buzzer_hal.c
    src/audio/modem_i2s_hal.c
    src/audio/nau88c22_codec.c
    src/audio/vibra_hal.c
    src/apps/battery_status_logic.c
    src/apps/calculator_app.c
    src/apps/game_common.c
    src/apps/call_divert_app.c
    src/apps/call_register_app.c
    src/apps/call_options_logic.c
    src/apps/calls_app.c
    src/apps/clock/app.c
    src/apps/clock/alarm.c
    src/apps/clock/editor.c
    src/apps/clock_alarm_logic.c
    src/apps/clock_date_logic.c
    src/apps/dialogs_app.c
    src/apps/games_app.c
    src/apps/logic_app.c
    src/apps/main_menu_app.c
    src/apps/memory_app.c
    src/apps/messages/app.c
    src/apps/messages/composer.c
    src/apps/messages/picture.c
    src/apps/net_monitor/app.c
    src/apps/net_monitor/logic.c
    src/apps/net_monitor/render.c
    src/apps/net_monitor/render_control.c
    src/apps/net_monitor/render_helpers.c
    src/apps/net_monitor/render_local.c
    src/apps/net_monitor/render_modem.c
    src/apps/net_monitor/render_radio.c
    src/apps/net_monitor/render_telephony.c
    src/apps/sim_presence_logic.c
    src/apps/pacman_app.c
    src/apps/phonebook_app.c
    src/apps/power_app.c
    src/apps/powerup_app.c
    src/apps/profiles_app.c
    src/apps/react_app.c
    src/apps/rotation_app.c
    src/apps/settings_app.c
    src/apps/service_codes_app.c
    src/apps/snake_app.c
    src/apps/standby_app.c
    src/apps/tones/app.c
    src/apps/tones/composer.c
    src/hal/accessory_hal.c
    src/services/battery_charge_logic.c
    src/hal/battery_hal.c
    src/hal/bq25171_stat.c
    src/hal/battery_gauge_logic.c
    src/hal/battery_poll_logic.c
    src/hal/battery_runtime_logic.c
    src/hal/charger_input_logic.c
    src/hal/board.c
    src/hal/board_irq_hal.c
    src/hal/board_safe_gpio.c
    src/hal/keypad.c
    src/hal/lcd_pcd8544.c
    src/hal/ltc2959_hal.c
    src/hal/ltc2959_regs.c
    src/hal/modem_power_monitor_hal.c
    src/hal/modem_power_monitor_logic.c
    src/hal/modem_uart_hal.c
    src/hal/power_button_hal.c
    src/hal/power_sleep_hal.c
    src/hal/power_sleep_wake_logic.c
    src/hal/rtc_alarm_hal.c
    src/hal/standby_sleep_hal.c
    src/hal/rv8803_hal.c
    src/hal/rv8803_regs.c
    src/hal/tca8418_hal.c
    src/hal/service_vbus_logic.c
    src/main.c
    src/services/backlight_calibration.c
    src/services/backlight_service.c
    src/services/battery_learning_logic.c
    src/services/battery_learning_service.c
    src/services/battery_charge_supervisor_logic.c
    src/services/battery_charge_supervisor_service.c
    src/services/board_diag_service.c
    src/services/call_forward_mmi.c
    src/services/charger_control_service.c
    src/services/core1_services.c
    ${SISU_DEBUG_CONSOLE_SOURCE}
    src/services/event_queue.c
    src/services/feature_gates.c
    src/services/key_utils.c
    src/services/lcd_calibration.c
    src/services/log.c
    src/services/modem_diag_engine.c
    src/services/modem_line_framer.c
    src/services/modem_line_parser.c
    src/services/modem_maintenance.c
    src/services/modem_phonebook_state.c
    src/services/modem_sms_direct.c
    src/services/modem_sms_protocol.c
    src/services/modem_sms_state.c
    src/services/modem_supplementary_state.c
    src/services/modem_service.c
    src/services/operator_name_db.c
    src/services/modem_call_model.c
    src/services/netmon_control_service.c
    src/services/netmon_diag_service.c
    src/services/modem_vendor_telit.c
    src/services/modem_vendor_telit_call.c
    src/services/modem_vendor_telit_diag.c
    src/services/modem_vendor_telit_maintenance.c
    src/services/modem_vendor_telit_parse.c
    src/services/modem_vendor_telit_operator.c
    src/services/modem_vendor_telit_provision.c
    src/services/modem_vendor_telit_sms.c
    src/services/modem_vendor_telit_tune.c
    src/services/phone_match.c
    src/services/power_sleep.c
    src/services/runtime_watchdog.c
    src/services/shared_3v8_service.c
    src/services/shared_irq_logic.c
    src/services/shared_irq_service.c
    src/services/standby_sleep.c
    src/services/standby_sleep_logic.c
    src/services/stack_monitor.c
    src/services/stack_monitor_logic.c
    src/services/strings.c
    src/services/power_sleep_logic.c
    src/services/sms_identity.c
    src/services/sms_picture_codec.c
    src/services/sms_deliver_codec.c
    src/services/sms_submit_codec.c
    src/services/sms_vvm_filter.c
    src/services/t9_service.c
    src/services/timebase.c
    src/services/usb_service.c
    src/storage/nvm_flash_hal.c
    src/storage/storage_journal.c
    src/storage/store_calls.c
    src/storage/store_battery_learning.c
    src/storage/store_battery_charge_supervisor.c
    src/storage/store_divert.c
    src/storage/store_pictures.c
    src/storage/store_service.c
    src/storage/store_settings.c
    src/storage/store_t9.c
    src/storage/store_tones.c
    src/storage/store_warranty.c
    src/ui/assets.c
    src/ui/framebuffer.c
    src/ui/menu_visible.c
    src/ui/signal_bars.c
    src/ui/status_chrome.c
    src/ui/text_layout.c
    src/ui/ui.c
    ${SISU_GENERATED_SOURCES}
)

target_include_directories(${SISU_FIRMWARE_TARGET} PRIVATE
    ${SISU_GENERATED_INC_DIR}
    include
    ${CMAKE_CURRENT_BINARY_DIR}/generated
)

# Build-time version stamp for the *#0000# service screen: regenerate
# generated/build_info.h (short git hash + build date) on every build via an
# always-run target. The script only rewrites the header when the values change,
# so service_codes_app.c recompiles only when the stamp actually changes.
set(SISU_BUILD_INFO_H ${CMAKE_CURRENT_BINARY_DIR}/generated/build_info.h)
add_custom_target(sisu_build_info
    BYPRODUCTS ${SISU_BUILD_INFO_H}
    COMMAND ${CMAKE_COMMAND}
        -DOUT=${SISU_BUILD_INFO_H}
        -DSRC_DIR=${CMAKE_CURRENT_LIST_DIR}
        -P ${CMAKE_CURRENT_LIST_DIR}/cmake/gen_build_info.cmake
    COMMENT "Generating build_info.h (git hash + build date)"
    VERBATIM)
add_dependencies(${SISU_FIRMWARE_TARGET} sisu_build_info)

target_compile_definitions(${SISU_FIRMWARE_TARGET} PRIVATE
    # Hardware-revision name shown on the *#0000# version screen -- a build
    # identity fact beside SISU_BUILD_HASH/DATE, so the app needs no HAL header.
    SISU_HW_REV_NAME="RevB2"
    # RP2350's default linker fixes each core's stack in one 4 KiB scratch
    # bank. MSPLIM catches a downward overrun before it can corrupt memory.
    PICO_USE_STACK_GUARDS=1
    PICO_STACK_SIZE=0x1000
    PICO_CORE1_STACK_SIZE=0x1000
    # GP28 has already qualified physical VBUS absent before teardown. The SDK
    # default 110 ms disconnect grace only stalls the 8 ms product loop here.
    PICO_STDIO_USB_DEINIT_DELAY_MS=0
    # Fix pico_stdio_usb's otherwise private worker IRQ so usb_service can
    # re-pend it from the product loop. RP2350 IRQs 46..51 are user IRQs; no
    # other production component claims one.
    PICO_STDIO_USB_LOW_PRIORITY_IRQ=51
    SISU_HW_REV_B2=1
    SISU_MODEM_VENDOR_TELIT=1
    SISU_MODEM_BACKEND_ENABLED=1
    SISU_RELEASE_BUILD=${SISU_RELEASE_BUILD_VALUE}
)

option(SISU_LTC2959_VOLTAGE_AUTHORITATIVE
       "Trust the P1-C-qualified LTC2959 voltage for user-facing battery policy"
       ON)
if(SISU_LTC2959_VOLTAGE_AUTHORITATIVE)
    target_compile_definitions(${SISU_FIRMWARE_TARGET} PRIVATE
        SISU_LTC2959_VOLTAGE_AUTHORITATIVE=1)
else()
    target_compile_definitions(${SISU_FIRMWARE_TARGET} PRIVATE
        SISU_LTC2959_VOLTAGE_AUTHORITATIVE=0)
endif()

target_compile_options(${SISU_FIRMWARE_TARGET} PRIVATE
    -Wall
    -Wextra
    -Werror
    -Wno-unused-parameter
    $<$<COMPILE_LANGUAGE:C>:-fstack-usage>
    $<$<COMPILE_LANGUAGE:C>:-fcallgraph-info=su>
    $<$<COMPILE_LANGUAGE:CXX>:-fstack-usage>
    $<$<COMPILE_LANGUAGE:CXX>:-fcallgraph-info=su>
)

target_link_libraries(${SISU_FIRMWARE_TARGET}
    pico_stdlib
    pico_multicore
    hardware_adc
    hardware_clocks
    hardware_dma
    hardware_gpio
    hardware_i2c
    hardware_irq
    hardware_pio
    hardware_powman
    hardware_pwm
    hardware_spi
    hardware_uart
    hardware_flash
    hardware_watchdog
    pico_flash
)

pico_generate_pio_header(${SISU_FIRMWARE_TARGET} ${CMAKE_CURRENT_LIST_DIR}/src/audio/audio_i2s.pio)
pico_generate_pio_header(${SISU_FIRMWARE_TARGET} ${CMAKE_CURRENT_LIST_DIR}/src/audio/modem_i2s.pio)
if(RELEASE)
    # Preserve the stable CMake target name for tooling, but make it impossible
    # to confuse the distributable artifact with the CDC-enabled service image.
    set_target_properties(${SISU_FIRMWARE_TARGET} PROPERTIES
        OUTPUT_NAME "${SISU_FIRMWARE_TARGET}_release")
    pico_enable_stdio_usb(${SISU_FIRMWARE_TARGET} 0)
else()
    pico_enable_stdio_usb(${SISU_FIRMWARE_TARGET} 1)
endif()
pico_enable_stdio_uart(${SISU_FIRMWARE_TARGET} 0)
pico_add_extra_outputs(${SISU_FIRMWARE_TARGET})

# Fail the build if code grows into the NVM storage region (last 128 KiB).
add_custom_command(TARGET ${SISU_FIRMWARE_TARGET} POST_BUILD
    COMMAND ${CMAKE_COMMAND}
        -DELF=$<TARGET_FILE:${SISU_FIRMWARE_TARGET}>
        -DNM=${CMAKE_NM}
        -P ${CMAKE_CURRENT_LIST_DIR}/cmake/check_flash_budget.cmake
    VERBATIM
    COMMENT "Checking firmware fits below the NVM storage region")

# The direct-call allowance leaves 512 bytes for nested IRQ frames (including
# an extended FP exception frame) and 512 bytes for indirect/library paths that
# GCC cannot connect statically. Runtime canaries independently cover both.
add_custom_command(TARGET ${SISU_FIRMWARE_TARGET} POST_BUILD
    COMMAND ${SISU_PYTHON3}
        ${CMAKE_CURRENT_LIST_DIR}/tools/check_stack_budget.py
        --input-dir
        ${CMAKE_CURRENT_BINARY_DIR}/CMakeFiles/${SISU_FIRMWARE_TARGET}.dir
        --root core0=main
        --root core1=core1_main
        --stack-size 4096
        --irq-reserve 512
        --uncertainty-reserve 512
        --report
        ${CMAKE_CURRENT_BINARY_DIR}/${SISU_FIRMWARE_TARGET}.stack.txt
    VERBATIM
    COMMENT "Checking core stack budgets")

if(RELEASE)
    add_custom_command(TARGET ${SISU_FIRMWARE_TARGET} POST_BUILD
        COMMAND ${CMAKE_COMMAND}
            -DELF=$<TARGET_FILE:${SISU_FIRMWARE_TARGET}>
            -DNM=${CMAKE_NM}
            -P ${CMAKE_CURRENT_LIST_DIR}/cmake/check_release_image.cmake
        VERBATIM
        COMMENT "Checking release image has no CDC/service-console linkage")
endif()

# Rev B2 bench artifacts use the modem-free NONE backend where they do not need
# the production Telit service.
option(SISU_BUILD_DIAGNOSTICS
    "Build the Rev B2 bench diagnostic images and the antenna tuner" OFF)

if(SISU_BUILD_DIAGNOSTICS)

function(configure_revb2_diag target)
    target_include_directories(${target} PRIVATE include)
    target_compile_definitions(${target} PRIVATE
        SISU_HW_REV_B2=1
        SISU_MODEM_VENDOR_NONE=1
        SISU_MODEM_BACKEND_ENABLED=0)
    target_compile_options(${target} PRIVATE
        -Wall
        -Wextra
        -Werror
        -Wno-unused-parameter
    )
    target_link_libraries(${target}
        pico_stdlib
        hardware_adc
        hardware_clocks
        hardware_gpio
        hardware_i2c
        hardware_irq
        hardware_pwm
        hardware_spi
    )
    pico_enable_stdio_usb(${target} 1)
    pico_enable_stdio_uart(${target} 0)
    pico_add_extra_outputs(${target})
endfunction()

# This artifact has no app, modem service, battery ADC, codec
# service, or core 1; it only establishes safe levels and exercises LCD/keypad
# plus read-only board observations.
add_executable(diag_revb2_safe
    src/diag/diag_revb2_safe.c
    src/hal/board.c
    src/hal/board_safe_gpio.c
    src/hal/keypad.c
    src/hal/lcd_pcd8544.c
    src/hal/tca8418_hal.c
    src/services/event_queue.c
    src/services/log.c
    src/services/timebase.c
    src/ui/assets.c
    src/ui/framebuffer.c
    ${SISU_GENERATED_SRC_DIR}/assets_data.c
)
configure_revb2_diag(diag_revb2_safe)

# Codec-only Rev B2 digital/analog audio diagnostic. It reuses the production
# codec and I2S HALs but has no modem service or modem-side DVI source.
add_executable(diag_i2s
    src/diag/diag_i2s.c
    src/audio/audio_bridge.c
    src/audio/audio_i2s_hal.c
    src/audio/nau88c22_codec.c
    src/hal/accessory_hal.c
    src/hal/board.c
    src/hal/board_safe_gpio.c
    src/hal/lcd_pcd8544.c
    src/hal/tca8418_hal.c
    src/services/log.c
    src/services/timebase.c
    src/ui/assets.c
    src/ui/framebuffer.c
    ${SISU_GENERATED_SRC_DIR}/assets_data.c
)
configure_revb2_diag(diag_i2s)
target_include_directories(diag_i2s PRIVATE
    ${CMAKE_CURRENT_BINARY_DIR}/generated)
target_link_libraries(diag_i2s
    hardware_dma
    hardware_pio)
pico_generate_pio_header(diag_i2s
    ${CMAKE_CURRENT_LIST_DIR}/src/audio/audio_i2s.pio)

# Manual rail-only diagnostic. GP6 is raised only after an explicit CDC
# command; GP38/GP39 remain deasserted.
add_executable(diag_revb2_power
    src/diag/diag_revb2_power.c
    src/hal/board.c
    src/hal/board_safe_gpio.c
    src/hal/modem_power_monitor_hal.c
    src/hal/modem_power_monitor_logic.c
)
configure_revb2_diag(diag_revb2_power)

# Telit electrical-characterization tool. This remains a standalone Rev B2
# diagnostic with the NONE backend; it cannot be linked into production.
add_executable(diag_telit
    src/diag/diag_telit.c
    src/diag/telit_diag_logic.c
    src/hal/board.c
    src/hal/board_safe_gpio.c
    src/hal/ltc2959_hal.c
    src/hal/ltc2959_regs.c
    src/hal/modem_power_monitor_hal.c
    src/hal/modem_power_monitor_logic.c
    src/services/log.c
    src/services/timebase.c
)
configure_revb2_diag(diag_telit)
target_link_libraries(diag_telit
    hardware_uart)

# Bench-only dynamic antenna tuner, including its source, tests, map, and
# qualified firmware artifact, is kept together outside production code.
add_subdirectory(tools/tune_dynamic_ant)

# Telit DVI/OAP bench artifact. It reuses the bounded Telit power/UART console
# and production audio HALs, but the bridge is available only through explicit
# diagnostic commands. Production voice capability remains unchanged.
add_executable(diag_telit_dvi
    src/diag/diag_telit.c
    src/diag/telit_diag_logic.c
    src/diag/telit_dvi_diag.c
    src/audio/audio_bridge.c
    src/audio/audio_i2s_hal.c
    src/audio/modem_i2s_hal.c
    src/audio/nau88c22_codec.c
    src/hal/accessory_hal.c
    src/hal/board.c
    src/hal/board_safe_gpio.c
    src/hal/ltc2959_hal.c
    src/hal/ltc2959_regs.c
    src/hal/modem_power_monitor_hal.c
    src/hal/modem_power_monitor_logic.c
    src/hal/tca8418_hal.c
    src/services/log.c
    src/services/timebase.c
)
configure_revb2_diag(diag_telit_dvi)
target_include_directories(diag_telit_dvi PRIVATE
    ${CMAKE_CURRENT_BINARY_DIR}/generated)
target_compile_definitions(diag_telit_dvi PRIVATE
    SISU_TELIT_DVI_DIAGNOSTIC=1)
target_link_libraries(diag_telit_dvi
    hardware_dma
    hardware_pio
    hardware_uart)
pico_generate_pio_header(diag_telit_dvi
    ${CMAKE_CURRENT_LIST_DIR}/src/audio/audio_i2s.pio)
pico_generate_pio_header(diag_telit_dvi
    ${CMAKE_CURRENT_LIST_DIR}/src/audio/modem_i2s.pio)
pico_generate_pio_header(diag_telit_dvi
    ${CMAKE_CURRENT_LIST_DIR}/src/diag/dvi_clock_probe.pio)

add_executable(diag_charger_adc
    src/diag/diag_charger_adc.c
    src/services/battery_charge_logic.c
    src/hal/battery_gauge_logic.c
    src/hal/battery_hal.c
    src/hal/bq25171_stat.c
    src/hal/battery_poll_logic.c
    src/hal/battery_runtime_logic.c
    src/hal/board.c
    src/hal/board_safe_gpio.c
    src/hal/charger_input_logic.c
    src/hal/ltc2959_hal.c
    src/hal/ltc2959_regs.c
    src/hal/tca8418_hal.c
    src/services/log.c
    src/services/timebase.c
)
configure_revb2_diag(diag_charger_adc)

add_executable(diag_ltc2959
    src/diag/diag_ltc2959.c
    src/hal/board.c
    src/hal/board_irq_hal.c
    src/hal/board_safe_gpio.c
    src/hal/ltc2959_hal.c
    src/hal/ltc2959_regs.c
    src/services/log.c
    src/services/timebase.c
)
configure_revb2_diag(diag_ltc2959)

add_executable(diag_shared_irq
    src/diag/diag_shared_irq.c
    src/hal/board.c
    src/hal/board_irq_hal.c
    src/hal/board_safe_gpio.c
    src/hal/ltc2959_hal.c
    src/hal/ltc2959_regs.c
    src/hal/rtc_alarm_hal.c
    src/hal/rv8803_hal.c
    src/hal/rv8803_regs.c
    src/hal/tca8418_hal.c
    src/services/log.c
    src/services/shared_irq_logic.c
    src/services/shared_irq_service.c
    src/services/timebase.c
)
configure_revb2_diag(diag_shared_irq)

endif() # SISU_BUILD_DIAGNOSTICS

# The always-run asset check (options and mode are defined before the
# firmware target). With firmware present the assets are (re)generated —
# an input stamp skips the ~20 s pipeline when nothing changed; without it
# the default selection builds from previously generated files if they
# exist, and the build fails with instructions when neither is present
# (custom selections always need the firmware). See firmware/README.md and
# tools/assetgen/gen_build.py.
if(SISU_ASSETS_EMIT_DIR)
    set(SISU_ASSETS_EMIT_ARGS --emit-dir ${SISU_ASSETS_EMIT_DIR})
else()
    set(SISU_ASSETS_EMIT_ARGS)
endif()
add_custom_target(sisu_assets
    COMMAND ${SISU_PYTHON3} ${CMAKE_CURRENT_SOURCE_DIR}/tools/assetgen/gen_build.py
            --stamp ${CMAKE_BINARY_DIR}/sisu_assets.stamp
            --work ${CMAKE_BINARY_DIR}/assetgen-work
            --langs ${SISU_ASSET_LANGS} --t9-langs ${SISU_T9_LANGS}
            ${SISU_ASSETS_EMIT_ARGS}
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    COMMENT "Checking original-firmware assets"
    VERBATIM)
add_dependencies(${SISU_FIRMWARE_TARGET} sisu_assets)
if(SISU_BUILD_DIAGNOSTICS)
    add_dependencies(diag_revb2_safe sisu_assets)
    add_dependencies(diag_i2s sisu_assets)
    add_dependencies(tune_dynamic_ant sisu_assets)
endif()
