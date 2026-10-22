
cmake_minimum_required(VERSION 3.22)

# Firmware version, shown on the boot screen and by `status`. Bump for releases.
set(PROJECT_VER "0.6.0")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# Only pull in the components the firmware uses; keeps build times short.
idf_build_set_property(MINIMAL_BUILD ON)

project(bx-tacho)
