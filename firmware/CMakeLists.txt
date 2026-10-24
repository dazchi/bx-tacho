# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Darren Chi

cmake_minimum_required(VERSION 3.22)

# Firmware version, shown on the boot screen and by `status`. Bump for releases.
set(PROJECT_VER "1.2.0")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# Only main and what it uses (keeps build times short), plus an optional Battle
# Pass extension: a component named bbp_ext in one of EXTRA_COMPONENT_DIRS
# (README "Optional extension"; interface in components/bbp_ext_api)
set(COMPONENTS main)
foreach(dir ${EXTRA_COMPONENT_DIRS})
    if(EXISTS "${dir}/bbp_ext/CMakeLists.txt")
        list(APPEND COMPONENTS bbp_ext)
        # Builds with one say so in their version: boot screen, status, web app
        set(PROJECT_VER "${PROJECT_VER}-bbp")
    endif()
endforeach()

project(bx-tacho)

# A copy of the app image named after its version (bx-tacho-v1.2.0.bin), the
# file to publish; idf.py flash keeps using bx-tacho.bin
idf_build_get_property(project_ver PROJECT_VER)
set(app_bin "${CMAKE_BINARY_DIR}/${CMAKE_PROJECT_NAME}.bin")
set(versioned_bin "${CMAKE_BINARY_DIR}/${CMAKE_PROJECT_NAME}-v${project_ver}.bin")
add_custom_command(OUTPUT "${versioned_bin}"
    COMMAND ${CMAKE_COMMAND} -E copy "${app_bin}" "${versioned_bin}"
    DEPENDS gen_project_binary "${app_bin}"
    COMMENT "Copying ${CMAKE_PROJECT_NAME}.bin to ${CMAKE_PROJECT_NAME}-v${project_ver}.bin"
    VERBATIM)
add_custom_target(versioned_bin ALL DEPENDS "${versioned_bin}")
