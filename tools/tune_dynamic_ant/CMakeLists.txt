# Standalone Rev B2 antenna-matching tool. It deliberately uses the NONE
# backend and cannot be linked into the production firmware target.

# The GENERATED source property is directory-scoped: a custom asset selection
# emits into the build tree, and on a clean configure the file does not exist
# yet, so it must be declared generated HERE (not only in the parent) before
# add_executable sees it.
set_source_files_properties(${SISU_GENERATED_SRC_DIR}/assets_data.c
    PROPERTIES GENERATED TRUE)
add_executable(tune_dynamic_ant
    main.c
    tuner_logic.c
    ${PROJECT_SOURCE_DIR}/src/diag/telit_diag_logic.c
    ${PROJECT_SOURCE_DIR}/src/hal/board.c
    ${PROJECT_SOURCE_DIR}/src/hal/board_safe_gpio.c
    ${PROJECT_SOURCE_DIR}/src/hal/lcd_pcd8544.c
    ${PROJECT_SOURCE_DIR}/src/hal/ltc2959_hal.c
    ${PROJECT_SOURCE_DIR}/src/hal/ltc2959_regs.c
    ${PROJECT_SOURCE_DIR}/src/hal/modem_power_monitor_hal.c
    ${PROJECT_SOURCE_DIR}/src/hal/modem_power_monitor_logic.c
    ${PROJECT_SOURCE_DIR}/src/hal/tca8418_hal.c
    ${PROJECT_SOURCE_DIR}/src/services/log.c
    ${PROJECT_SOURCE_DIR}/src/services/timebase.c
    ${PROJECT_SOURCE_DIR}/src/ui/assets.c
    ${PROJECT_SOURCE_DIR}/src/ui/framebuffer.c
    ${SISU_GENERATED_SRC_DIR}/assets_data.c
)

configure_revb2_diag(tune_dynamic_ant)
target_include_directories(tune_dynamic_ant PRIVATE
    ${SISU_GENERATED_INC_DIR}
    ${PROJECT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_LIST_DIR})
target_compile_definitions(tune_dynamic_ant PRIVATE
    SISU_TELIT_TUNER_DIAGNOSTIC=1)
target_link_libraries(tune_dynamic_ant
    hardware_uart)
