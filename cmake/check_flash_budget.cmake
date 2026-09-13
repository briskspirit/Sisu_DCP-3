# Build-time guard: the firmware's flash-resident image (code + rodata + data
# init) must not grow into the NVM storage region reserved at the top of flash.
# Storage lives in the last NVM_RESERVED_BYTES of PICO_FLASH_SIZE_BYTES (see
# src/storage/nvm_flash_hal.c); code grows up from XIP_BASE. If they ever meet,
# a flashed build would silently corrupt saved settings/SMS/etc -- so fail the
# build instead. Keep the two numbers below in sync with nvm_flash_hal.c.
#
# Invoked as a POST_BUILD step with -DELF=<path> -DNM=<arm-none-eabi-nm>.

set(XIP_BASE 0x10000000)
set(PICO_FLASH_SIZE 0x200000)   # 2 MiB (RP2354B stacked flash)
set(NVM_RESERVED  0x20000)      # 128 KiB storage region

math(EXPR STORAGE_START "${XIP_BASE} + ${PICO_FLASH_SIZE} - ${NVM_RESERVED}" OUTPUT_FORMAT HEXADECIMAL)

execute_process(
    COMMAND "${NM}" "${ELF}"
    OUTPUT_VARIABLE NM_OUT
    RESULT_VARIABLE NM_RC)
if(NOT NM_RC EQUAL 0)
    message(FATAL_ERROR "check_flash_budget: nm failed on ${ELF}")
endif()

string(REGEX MATCH "([0-9a-fA-F]+) R __flash_binary_end" _m "${NM_OUT}")
if(NOT CMAKE_MATCH_1)
    string(REGEX MATCH "([0-9a-fA-F]+) T __flash_binary_end" _m "${NM_OUT}")
endif()
if(NOT CMAKE_MATCH_1)
    message(FATAL_ERROR "check_flash_budget: __flash_binary_end not found in ${ELF}")
endif()

set(BIN_END "0x${CMAKE_MATCH_1}")
math(EXPR USED "${BIN_END} - ${XIP_BASE}")
math(EXPR AVAIL "${STORAGE_START} - ${XIP_BASE}")
math(EXPR FREE "${STORAGE_START} - ${BIN_END}")

if(BIN_END GREATER_EQUAL STORAGE_START)
    message(FATAL_ERROR
        "Firmware overflows into the NVM storage region!\n"
        "  flash image ends at ${BIN_END}, storage starts at ${STORAGE_START}.\n"
        "  Reduce code/rodata or shrink NVM_FLASH_RESERVED_BYTES (fewer store units).")
endif()

math(EXPR USED_KB "${USED} / 1024")
math(EXPR AVAIL_KB "${AVAIL} / 1024")
math(EXPR FREE_KB "${FREE} / 1024")
message(STATUS "flash budget: code ${USED_KB} KiB of ${AVAIL_KB} KiB before storage (${FREE_KB} KiB free)")
