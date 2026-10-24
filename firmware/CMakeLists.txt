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
