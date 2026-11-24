# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Darren Chi
#
# Host tests of the load classifier (firmware/main/load_classify.c), built for
# the PC with clang (MSYS2 CLANG64 on Windows, or Linux):
#
#   cmake -S firmware/test/host -B firmware/test/host/build -G Ninja
#   cmake --build firmware/test/host/build
#   ctest --test-dir firmware/test/host/build --output-on-failure
#
# Unity comes from ESP-IDF (IDF_PATH), or -DUNITY_DIR=<its src folder>, or is
# downloaded. With a libFuzzer-capable clang (Linux), -DFUZZ_LIBFUZZER=ON
# builds the fuzz target for libFuzzer instead of fuzz_main.c's random runs.
cmake_minimum_required(VERSION 3.20)
project(bx_tacho_host_tests C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
option(FUZZ_LIBFUZZER "Build the fuzz target for libFuzzer" OFF)
set(FUZZ_RUNS 20000 CACHE STRING "Random inputs the fuzz test runs under ctest")
set(FUZZ_SEED 1 CACHE STRING "Seed of the fuzz test under ctest")

set(FW_MAIN ${CMAKE_CURRENT_SOURCE_DIR}/../../main)

# ---- Unity
if(NOT UNITY_DIR AND DEFINED ENV{IDF_PATH} AND EXISTS "$ENV{IDF_PATH}/components/unity/unity/src/unity.c")
    set(UNITY_DIR "$ENV{IDF_PATH}/components/unity/unity/src")
endif()
if(UNITY_DIR)
    message(STATUS "Unity: ${UNITY_DIR}")
    add_library(unity STATIC ${UNITY_DIR}/unity.c)
    target_include_directories(unity PUBLIC ${UNITY_DIR})
else()
    # Downloaded: Unity's own CMake project makes the `unity` library target
    include(FetchContent)
    FetchContent_Declare(unity
        URL https://github.com/ThrowTheSwitch/Unity/archive/refs/tags/v2.6.0.tar.gz)
    FetchContent_MakeAvailable(unity)
    message(STATUS "Unity: downloaded, ${unity_SOURCE_DIR}")
endif()

# ---- the classifier as the firmware builds it, with its outside faked
add_library(classifier STATIC
    ${FW_MAIN}/load_classify.c
    fake.c
    sim.c)
target_include_directories(classifier PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/stubs
    ${FW_MAIN})

set(WARNINGS -Wall -Wextra -Werror -Wno-unused-parameter)
# Overflow, division by zero, bad shifts, out of bounds: stop at the first
set(SANITIZE -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer -g)
foreach(t unity classifier)
    target_compile_options(${t} PRIVATE ${SANITIZE})
endforeach()
target_compile_options(classifier PRIVATE ${WARNINGS})

# ---- unit tests
add_executable(test_load_classify test_load_classify.c)
target_link_libraries(test_load_classify PRIVATE classifier unity)
target_compile_options(test_load_classify PRIVATE ${WARNINGS} ${SANITIZE})
target_link_options(test_load_classify PRIVATE ${SANITIZE})

enable_testing()
add_test(NAME load_classify COMMAND test_load_classify)

# ---- fuzzing
if(FUZZ_LIBFUZZER)
    add_executable(fuzz_load_classify fuzz_load_classify.c)
    target_compile_options(fuzz_load_classify PRIVATE ${WARNINGS} ${SANITIZE} -fsanitize=fuzzer)
    target_compile_options(classifier PRIVATE -fsanitize=fuzzer-no-link)
    target_link_options(fuzz_load_classify PRIVATE ${SANITIZE} -fsanitize=fuzzer)
    add_test(NAME fuzz_load_classify COMMAND fuzz_load_classify -runs=${FUZZ_RUNS} -seed=${FUZZ_SEED})
else()
    add_executable(fuzz_load_classify fuzz_load_classify.c fuzz_main.c)
    target_compile_options(fuzz_load_classify PRIVATE ${WARNINGS} ${SANITIZE})
    target_link_options(fuzz_load_classify PRIVATE ${SANITIZE})
    add_test(NAME fuzz_load_classify COMMAND fuzz_load_classify -runs ${FUZZ_RUNS} -seed ${FUZZ_SEED})
endif()
target_link_libraries(fuzz_load_classify PRIVATE classifier)
