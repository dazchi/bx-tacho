cmake_minimum_required(VERSION 3.22)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# Only pull in the components the firmware uses; keeps build times short.
idf_build_set_property(MINIMAL_BUILD ON)

project(bx-tacho)
