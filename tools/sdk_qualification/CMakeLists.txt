cmake_minimum_required(VERSION 3.13)

get_filename_component(SISU_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
list(APPEND PICO_BOARD_HEADER_DIRS "${SISU_ROOT}/boards")
set(PICO_BOARD sisu_sse_revb2 CACHE STRING "Target board" FORCE)
include("${SISU_ROOT}/cmake/pico_sdk_import.cmake")
project(sisu_sdk_qualification C CXX ASM)
set(CMAKE_C_STANDARD 11)
pico_sdk_init()

add_subdirectory("${PICO_SDK_PATH}/test/pico_test" pico_test)
add_subdirectory("${PICO_SDK_PATH}/test/pico_time_test" pico_time_test)
add_subdirectory("${PICO_SDK_PATH}/test/pico_sync_test" pico_sync_test)
add_subdirectory("${PICO_SDK_PATH}/test/short_sleep_test" short_sleep_test)

add_executable(powman_handoff_test powman_handoff_test.c)
target_link_libraries(powman_handoff_test PRIVATE pico_stdlib hardware_powman)
pico_add_extra_outputs(powman_handoff_test)

foreach(target pico_time_test pico_time_test_sw pico_sync_test pico_sync_test_sw
               short_sleep_test short_sleep_test_sw powman_handoff_test)
    target_sources(${target} PRIVATE runner.c "${SISU_ROOT}/src/hal/board_safe_gpio.c")
    target_include_directories(${target} PRIVATE "${SISU_ROOT}/include")
    target_compile_definitions(${target} PRIVATE
        SISU_HW_REV_B2=1 SISU_MODEM_BACKEND_ENABLED=0
        SDK_TEST_NAME="${target}"
        PICO_STACK_SIZE=0x1000 PICO_CORE1_STACK_SIZE=0x1000
        PICO_USE_STACK_GUARDS=1)
    target_link_libraries(${target} PRIVATE pico_multicore hardware_watchdog)
    target_link_options(${target} PRIVATE
        -Wl,--wrap=main -Wl,--wrap=stdio_init_all -Wl,--wrap=stdio_deinit_all)
    pico_enable_stdio_usb(${target} 1)
    pico_enable_stdio_uart(${target} 0)
endforeach()
