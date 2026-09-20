# Derived from the Raspberry Pi Pico SDK's external/pico_sdk_import.cmake.
# Copyright (c) 2020 Raspberry Pi (Trading) Ltd.
# SPDX-License-Identifier: BSD-3-Clause
#
# This is a small copy of the standard Pico SDK import helper. It keeps this
# firmware tree buildable with either PICO_SDK_PATH or CMake-managed SDK fetch.

if(DEFINED ENV{PICO_SDK_PATH} AND NOT PICO_SDK_PATH)
    set(PICO_SDK_PATH $ENV{PICO_SDK_PATH})
endif()

set(PICO_SDK_PATH "${PICO_SDK_PATH}" CACHE PATH "Path to the Raspberry Pi Pico SDK")
set(PICO_SDK_FETCH_FROM_GIT "${PICO_SDK_FETCH_FROM_GIT}" CACHE BOOL "Fetch pico-sdk from git")
set(PICO_SDK_FETCH_FROM_GIT_TAG "${PICO_SDK_FETCH_FROM_GIT_TAG}" CACHE STRING "pico-sdk git tag")
set(PICO_SDK_FETCH_FROM_GIT_PATH "${PICO_SDK_FETCH_FROM_GIT_PATH}" CACHE PATH "pico-sdk fetch path")
set(SISU_ALLOW_UNQUALIFIED_PICO_SDK "${SISU_ALLOW_UNQUALIFIED_PICO_SDK}" CACHE BOOL
    "Allow an SDK other than the clean, qualified pico-sdk commit (development only)")

if(NOT SISU_PICO_SDK_QUALIFIED_COMMIT)
    set(SISU_PICO_SDK_QUALIFIED_COMMIT
        "079c6f39023649b154152db30f1d781e884879bc")
endif()

if(NOT PICO_SDK_PATH)
    if(PICO_SDK_FETCH_FROM_GIT)
        include(FetchContent)
        set(FETCHCONTENT_BASE_DIR_SAVE ${FETCHCONTENT_BASE_DIR})
        if(PICO_SDK_FETCH_FROM_GIT_PATH)
            get_filename_component(FETCHCONTENT_BASE_DIR "${PICO_SDK_FETCH_FROM_GIT_PATH}" REALPATH BASE_DIR "${CMAKE_SOURCE_DIR}")
        endif()
        if(NOT PICO_SDK_FETCH_FROM_GIT_TAG)
            set(PICO_SDK_FETCH_FROM_GIT_TAG "${SISU_PICO_SDK_QUALIFIED_COMMIT}")
        endif()
        FetchContent_Populate(
            pico_sdk
            QUIET
            GIT_REPOSITORY https://github.com/raspberrypi/pico-sdk
            GIT_TAG ${PICO_SDK_FETCH_FROM_GIT_TAG}
            GIT_SUBMODULES_RECURSE FALSE
            SOURCE_DIR ${FETCHCONTENT_BASE_DIR}/pico_sdk-src
            BINARY_DIR ${FETCHCONTENT_BASE_DIR}/pico_sdk-build
            SUBBUILD_DIR ${FETCHCONTENT_BASE_DIR}/pico_sdk-subbuild
        )
        set(PICO_SDK_PATH ${pico_sdk_SOURCE_DIR})
        set(FETCHCONTENT_BASE_DIR ${FETCHCONTENT_BASE_DIR_SAVE})
    else()
        message(FATAL_ERROR "Set PICO_SDK_PATH or enable PICO_SDK_FETCH_FROM_GIT")
    endif()
endif()

get_filename_component(PICO_SDK_PATH "${PICO_SDK_PATH}" REALPATH BASE_DIR "${CMAKE_BINARY_DIR}")
if(NOT EXISTS "${PICO_SDK_PATH}/pico_sdk_init.cmake")
    message(FATAL_ERROR "PICO_SDK_PATH does not contain pico_sdk_init.cmake: ${PICO_SDK_PATH}")
endif()

# `git -C` searches parent directories. Requiring .git at the SDK root avoids
# falsely accepting an arbitrary pico_sdk_init.cmake nested inside this repo.
set(_sisu_sdk_error "")
if(NOT EXISTS "${PICO_SDK_PATH}/.git")
    set(_sisu_sdk_error
        "${PICO_SDK_PATH} is not a Git checkout rooted at the SDK directory")
else()
    find_program(SISU_GIT_EXECUTABLE NAMES git)
    if(NOT SISU_GIT_EXECUTABLE)
        set(_sisu_sdk_error "git is required to verify the pico-sdk revision")
    else()
        execute_process(
            COMMAND "${SISU_GIT_EXECUTABLE}" -C "${PICO_SDK_PATH}" rev-parse HEAD
            RESULT_VARIABLE _sisu_sdk_head_result
            OUTPUT_VARIABLE _sisu_sdk_head
            ERROR_VARIABLE _sisu_sdk_head_error
            OUTPUT_STRIP_TRAILING_WHITESPACE)
        if(NOT _sisu_sdk_head_result EQUAL 0)
            set(_sisu_sdk_error
                "could not read pico-sdk HEAD: ${_sisu_sdk_head_error}")
        elseif(NOT _sisu_sdk_head STREQUAL SISU_PICO_SDK_QUALIFIED_COMMIT)
            set(_sisu_sdk_error
                "pico-sdk HEAD is ${_sisu_sdk_head}, expected ${SISU_PICO_SDK_QUALIFIED_COMMIT}")
        else()
            execute_process(
                COMMAND "${SISU_GIT_EXECUTABLE}" -C "${PICO_SDK_PATH}"
                        status --porcelain --untracked-files=normal
                RESULT_VARIABLE _sisu_sdk_status_result
                OUTPUT_VARIABLE _sisu_sdk_status
                ERROR_VARIABLE _sisu_sdk_status_error
                OUTPUT_STRIP_TRAILING_WHITESPACE)
            if(NOT _sisu_sdk_status_result EQUAL 0)
                set(_sisu_sdk_error
                    "could not inspect pico-sdk worktree: ${_sisu_sdk_status_error}")
            elseif(NOT _sisu_sdk_status STREQUAL "")
                set(_sisu_sdk_error
                    "pico-sdk worktree is not clean: ${_sisu_sdk_status}")
            endif()
        endif()
    endif()
endif()

if(NOT _sisu_sdk_error STREQUAL "")
    if(SISU_ALLOW_UNQUALIFIED_PICO_SDK)
        message(WARNING
            "Unqualified pico-sdk accepted by explicit override: ${_sisu_sdk_error}")
    else()
        message(FATAL_ERROR
            "Unqualified pico-sdk: ${_sisu_sdk_error}. Use the qualified checkout "
            "or set SISU_ALLOW_UNQUALIFIED_PICO_SDK=ON for an intentional "
            "development-only build.")
    endif()
else()
    message(STATUS
        "Qualified pico-sdk commit: ${SISU_PICO_SDK_QUALIFIED_COMMIT}")
endif()

set(PICO_SDK_PATH ${PICO_SDK_PATH} CACHE PATH "Path to the Raspberry Pi Pico SDK" FORCE)
include("${PICO_SDK_PATH}/pico_sdk_init.cmake")
