# SPDX-License-Identifier: GPL-3.0-only
# SPDX-FileCopyrightText: 2026 Paul R. Decker
#
# channel-sim: host build of the vendored pathsim channel simulator (Phase 5a).
# Guide: docs/modules/pathsim.md.
#
# A standalone project, deliberately outside the root waveform-generator build:
# firmware and host builds never need pathsim, and this build never touches them.
#
#   cmake --preset pathsim-host          (from channel-sim/; or the command below)
#   cmake -S channel-sim -B build/main/pathsim-host -G "MinGW Makefiles"
#   cmake --build build/main/pathsim-host
#
# pathsim (third_party/pathsim, bubnikv/pathsim, GNU GPL v3; PathSim by
# Moe Wheatley AE4JY, reworked by Vojtech Bubnik OK1IAK) stays pristine.
# cmake/pathsim_vendor.cmake copies the checked-out submodule sources into the build tree and
# applies the HF lane's rev-6 patch there (pathsim-src), and in a second copy
# also waveform-generator's draw-order patch (pathsim-chain-src). Targets:
#   pathsim_ref       the rev-6 pathsim's own command-line program, built with
#                     the recipe of the lane's shipped pathsim.exe (77461211...)
#   pathsim_lib       pathsim's library sources from the chain copy, with the RNG shim
#   channel_sim       the owned chain driver (core/), platform-neutral
#   pathsim_chain     the chain driver's host CLI (cli/)
#   channel_sim_tests, wav_file_tests, ucrt_rand_tests   unit tests (ctest), with
#   pathsim_chain_cli_tests and gate_script_tests (Python)
# channel-sim/gates/ compares them with the lane's binaries and outputs.

cmake_minimum_required(VERSION 3.25)

if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
    set(CMAKE_TOOLCHAIN_FILE "${CMAKE_CURRENT_LIST_DIR}/cmake/winlibs-host.cmake")
endif()

project(channel_sim
    DESCRIPTION "Host build of the vendored pathsim HF channel simulator"
    LANGUAGES CXX)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type; the pathsim reference recipe is Release" FORCE)
endif()
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

cmake_path(GET CMAKE_CURRENT_SOURCE_DIR PARENT_PATH WFG_ROOT)
include(cmake/pathsim_vendor.cmake)

# --- Reference CLI ---------------------------------------------------------
# The lane built 7746121 with CMake's Release defaults for GCC 14.2.0
# (-O3 -DNDEBUG), no -std (GCC 14's default, gnu++17), no warning, ISA or
# floating-point flags, upstream's source order, MinGW Makefiles and a
# dynamically linked libstdc++.
# pathsim_ref uses exactly that: no target options are added, and a different
# compiler or flag set is reported, because bit parity is then unproven.
# main.cpp needs exceptions (it catches cxxopts errors) and RTTI (cxxopts uses
# dynamic_cast), which GCC enables by default.
set(PATHSIM_REFERENCE_COMPILER_ID GNU)
set(PATHSIM_REFERENCE_COMPILER_VERSION 14.2.0)
set(PATHSIM_REFERENCE_FLAGS "-O3 -DNDEBUG")

add_executable(pathsim_ref ${PATHSIM_REFERENCE_SOURCES})
target_include_directories(pathsim_ref PRIVATE "${PATHSIM_PATCHED_DIR}")

string(STRIP "${CMAKE_CXX_FLAGS} ${CMAKE_CXX_FLAGS_RELEASE}" pathsim_ref_flags)
set(pathsim_ref_recipe_matches TRUE)
if(NOT MINGW OR NOT CMAKE_GENERATOR STREQUAL "MinGW Makefiles"
   OR NOT CMAKE_CXX_COMPILER_ID STREQUAL PATHSIM_REFERENCE_COMPILER_ID
   OR NOT CMAKE_CXX_COMPILER_VERSION VERSION_EQUAL PATHSIM_REFERENCE_COMPILER_VERSION)
    set(pathsim_ref_recipe_matches FALSE)
    message(WARNING "pathsim_ref: compiler/platform/generator is ${CMAKE_CXX_COMPILER_ID} "
        "${CMAKE_CXX_COMPILER_VERSION} on ${CMAKE_SYSTEM_NAME} with ${CMAKE_GENERATOR}; the lane's "
        "pathsim.exe used WinLibs MinGW-W64 GCC ${PATHSIM_REFERENCE_COMPILER_VERSION} with MinGW Makefiles. "
        "Bit parity is unproven.")
endif()
if(NOT CMAKE_BUILD_TYPE STREQUAL "Release" OR NOT pathsim_ref_flags STREQUAL PATHSIM_REFERENCE_FLAGS)
    set(pathsim_ref_recipe_matches FALSE)
    message(WARNING "pathsim_ref: build type '${CMAKE_BUILD_TYPE}' with flags '${pathsim_ref_flags}'; the lane's recipe "
        "is Release with '${PATHSIM_REFERENCE_FLAGS}'. Bit parity is unproven.")
endif()

# Run beside the toolchain's own runtime DLLs, so another libstdc++ on PATH
# cannot shadow the one the lane's binary loads.
function(channel_sim_copy_runtime target)
    if(MINGW)
        cmake_path(GET CMAKE_CXX_COMPILER PARENT_PATH toolchain_bin)
        foreach(dll IN ITEMS libstdc++-6.dll libgcc_s_seh-1.dll libwinpthread-1.dll)
            if(EXISTS "${toolchain_bin}/${dll}")
                add_custom_command(TARGET ${target} POST_BUILD
                    COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${toolchain_bin}/${dll}" "$<TARGET_FILE_DIR:${target}>"
                    VERBATIM)
            endif()
        endforeach()
    endif()
endfunction()
channel_sim_copy_runtime(pathsim_ref)

# The recipe record the gate scripts put into their reports.
if(pathsim_ref_recipe_matches)
    set(pathsim_ref_recipe_matches_json true)
else()
    set(pathsim_ref_recipe_matches_json false)
endif()
file(WRITE "${CMAKE_BINARY_DIR}/pathsim_ref.recipe.json"
"{
  \"pathsim_commit\": \"${PATHSIM_COMMIT}\",
  \"patch\": \"patches/${PATHSIM_PATCH_NAME}.patch\",
  \"compiler\": \"${CMAKE_CXX_COMPILER}\",
  \"compiler_id\": \"${CMAKE_CXX_COMPILER_ID}\",
  \"compiler_version\": \"${CMAKE_CXX_COMPILER_VERSION}\",
  \"generator\": \"${CMAKE_GENERATOR}\",
  \"build_type\": \"${CMAKE_BUILD_TYPE}\",
  \"cxx_flags\": \"${pathsim_ref_flags}\",
  \"reference_recipe\": \"WinLibs MinGW-W64 ${PATHSIM_REFERENCE_COMPILER_ID} ${PATHSIM_REFERENCE_COMPILER_VERSION}, MinGW Makefiles, Release, ${PATHSIM_REFERENCE_FLAGS}\",
  \"recipe_matches_reference\": ${pathsim_ref_recipe_matches_json}
}
")

message(STATUS "pathsim_ref: ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}, ${CMAKE_BUILD_TYPE}, '${pathsim_ref_flags}'")

# --- Chain build ------------------------------------------------------------
# Owned code (channel-sim's own sources): C++23 without extensions, warnings as
# errors, no exceptions or RTTI, no floating-point contraction (the RT1170 has
# FMA; x86-64's baseline ISA does not, so the host code is unchanged by it).
set(CHANNEL_SIM_OWNED_FLAGS -Wall -Wextra -Wpedantic -Werror -fno-exceptions -fno-rtti -ffp-contract=off)
function(channel_sim_owned target)
    set_target_properties(${target} PROPERTIES CXX_STANDARD 23 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
    target_compile_options(${target} PRIVATE ${CHANNEL_SIM_OWNED_FLAGS})
    target_include_directories(${target} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}" "${WFG_ROOT}")
endfunction()

# The UCRT-compatible generator behind PathSim::rand() (rng/pathsim_rng.cpp).
add_library(pathsim_rng STATIC rng/pathsim_rng.cpp)
channel_sim_owned(pathsim_rng)

# pathsim's six library sources from the chain copy (rev-6 plus
# patches/pathsim-wfg-embedded.patch, which fixes the order of each pair of
# random draws for every compiler), compiled with the reference flags (Release
# -O3 -DNDEBUG, GNU dialect: Path.cpp needs M_PI from <math.h>), the embedded
# profile (-fno-exceptions -fno-rtti -ffp-contract=off), pathsim's own warning
# level (none added, no -Werror) and the force-included RNG shim. Both the patch
# and the shim modify pathsim; patches/pathsim-wfg-embedded.notice.md is their
# dated notice. main.cpp, AudioFile.h and cxxopts.h are not part of it.
set(PATHSIM_LIB_FLAGS -fno-exceptions -fno-rtti -ffp-contract=off)
set(PATHSIM_RNG_SHIM "${CMAKE_CURRENT_SOURCE_DIR}/rng/pathsim_rng_shim.hpp")
add_library(pathsim_lib STATIC ${PATHSIM_LIBRARY_SOURCES})
set_target_properties(pathsim_lib PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS ON)
target_include_directories(pathsim_lib SYSTEM PUBLIC "${PATHSIM_CHAIN_DIR}")
target_include_directories(pathsim_lib PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_options(pathsim_lib PRIVATE ${PATHSIM_LIB_FLAGS} "SHELL:-include \"${PATHSIM_RNG_SHIM}\"")
set_property(SOURCE ${PATHSIM_LIBRARY_SOURCES} APPEND PROPERTY OBJECT_DEPENDS "${PATHSIM_RNG_SHIM}")
target_link_libraries(pathsim_lib PUBLIC pathsim_rng)
# The shim must have taken every draw: pathsim's objects reference PathSim::rand()
# and neither the C runtime's rand nor srand.
add_custom_command(TARGET pathsim_lib POST_BUILD
    COMMAND "${CMAKE_COMMAND}" "-DNM=${CMAKE_NM}" "-DLIBRARY=$<TARGET_FILE:pathsim_lib>"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/check_pathsim_rng.cmake"
    VERBATIM)

# The chain driver: platform-neutral, heap-free except inside pathsim's classes.
add_library(channel_sim STATIC
    core/pcm.cpp
    core/channel_spec.cpp
    core/pathsim_driver.cpp
    core/lane_resampler.cpp
    core/lane_chain.cpp
    "${WFG_ROOT}/common/status.cpp")
channel_sim_owned(channel_sim)
target_include_directories(channel_sim PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}" "${WFG_ROOT}")
target_link_libraries(channel_sim PUBLIC pathsim_lib)

add_executable(pathsim_chain cli/pathsim_chain.cpp cli/options.cpp cli/wav_file.cpp "${WFG_ROOT}/common/host_critical_error.cpp")
channel_sim_owned(pathsim_chain)
target_link_libraries(pathsim_chain PRIVATE channel_sim)
channel_sim_copy_runtime(pathsim_chain)
# File names reach main() as written. The WinLibs MinGW-w64 runtime expands '*'
# and '?' in the command line first (its libmingw32 sets _dowildcard to -1): an
# output named in-4*.wav became in-48k.wav and overwrote it, and a "\\?\" name
# was mangled. The runtime's own CRT_noglob.o defines _dowildcard as 0, and an
# object on the link line comes before that library default. (Defining it in
# owned source would declare a reserved identifier, which the clang-tidy profile
# fails.) pathsim_ref keeps the expansion: it is the lane's pathsim.exe rebuilt,
# image for image.
if(MINGW)
    execute_process(COMMAND "${CMAKE_CXX_COMPILER}" -print-file-name=CRT_noglob.o
        OUTPUT_VARIABLE channel_sim_noglob_object OUTPUT_STRIP_TRAILING_WHITESPACE)
    if(NOT IS_ABSOLUTE "${channel_sim_noglob_object}" OR NOT EXISTS "${channel_sim_noglob_object}")
        message(FATAL_ERROR "The MinGW runtime has no CRT_noglob.o ('${channel_sim_noglob_object}'): "
            "pathsim_chain would expand wildcards in its file names.")
    endif()
    cmake_path(NORMAL_PATH channel_sim_noglob_object)
    target_link_libraries(pathsim_chain PRIVATE "${channel_sim_noglob_object}")
endif()

enable_testing()
add_executable(channel_sim_tests tests/channel_sim_tests.cpp)
channel_sim_owned(channel_sim_tests)
target_link_libraries(channel_sim_tests PRIVATE channel_sim)
channel_sim_copy_runtime(channel_sim_tests)
add_test(NAME channel_sim_tests COMMAND channel_sim_tests)
# The host CLI's WAV reader on malformed files (writes and removes a scratch file
# in the build directory).
add_executable(wav_file_tests tests/wav_file_tests.cpp cli/wav_file.cpp)
channel_sim_owned(wav_file_tests)
target_link_libraries(wav_file_tests PRIVATE channel_sim)
channel_sim_copy_runtime(wav_file_tests)
add_test(NAME wav_file_tests COMMAND wav_file_tests WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")
# The generator against this host's C runtime: only a Microsoft runtime (UCRT,
# msvcrt) is the reference.
if(WIN32)
    add_executable(ucrt_rand_tests tests/ucrt_rand_tests.cpp)
    channel_sim_owned(ucrt_rand_tests)
    channel_sim_copy_runtime(ucrt_rand_tests)
    add_test(NAME ucrt_rand_tests COMMAND ucrt_rand_tests)
endif()
# The built CLI's refusals (an output that is its input, --resample-gain-db on an
# 8 kHz input) and the gate scripts' verdicts and arguments, in Python.
find_package(Python3 COMPONENTS Interpreter)
if(Python3_Interpreter_FOUND)
    add_test(NAME pathsim_chain_cli_tests
        COMMAND "${Python3_EXECUTABLE}" -B "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_pathsim_chain_cli.py"
                --chain "$<TARGET_FILE:pathsim_chain>")
    add_test(NAME gate_script_tests
        COMMAND "${Python3_EXECUTABLE}" -B "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_gate_scripts.py")
else()
    message(WARNING "No Python 3 interpreter: the CLI and gate script tests are not registered.")
endif()

string(JOIN " " chain_owned_flags ${CHANNEL_SIM_OWNED_FLAGS})
string(JOIN " " chain_pathsim_flags ${PATHSIM_LIB_FLAGS})
file(WRITE "${CMAKE_BINARY_DIR}/pathsim_chain.recipe.json"
"{
  \"pathsim_commit\": \"${PATHSIM_COMMIT}\",
  \"chain_patch\": \"patches/${PATHSIM_CHAIN_PATCH_NAME}.patch\",
  \"pathsim_sources\": \"${PATHSIM_CHAIN_DIR}\",
  \"compiler\": \"${CMAKE_CXX_COMPILER}\",
  \"compiler_version\": \"${CMAKE_CXX_COMPILER_VERSION}\",
  \"build_type\": \"${CMAKE_BUILD_TYPE}\",
  \"cxx_flags\": \"${pathsim_ref_flags}\",
  \"pathsim_lib\":\"-std=gnu++17 ${chain_pathsim_flags} -include rng/pathsim_rng_shim.hpp\",
  \"owned\": \"-std=c++23 ${chain_owned_flags}\"
}
")
