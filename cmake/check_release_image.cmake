if(NOT DEFINED ELF OR NOT EXISTS "${ELF}")
    message(FATAL_ERROR "release-image check: ELF is missing: ${ELF}")
endif()
if(NOT DEFINED NM OR NOT EXISTS "${NM}")
    message(FATAL_ERROR "release-image check: nm is missing: ${NM}")
endif()

execute_process(
    COMMAND "${NM}" -C "${ELF}"
    RESULT_VARIABLE nm_result
    OUTPUT_VARIABLE symbols
    ERROR_VARIABLE nm_error)
if(NOT nm_result EQUAL 0)
    message(FATAL_ERROR "release-image check: nm failed: ${nm_error}")
endif()

set(forbidden_symbol_patterns
    "debug_console_"
    "stdio_usb_"
    "tud_"
    "tusb_"
    "usbd_")
foreach(pattern IN LISTS forbidden_symbol_patterns)
    if(symbols MATCHES "(^|\\n)[^\\n]*${pattern}")
        message(FATAL_ERROR
            "release-image check: forbidden CDC/service symbol matches ${pattern}")
    endif()
endforeach()
