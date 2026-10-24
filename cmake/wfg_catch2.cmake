# SPDX-License-Identifier: GPL-3.0-only
# SPDX-FileCopyrightText: 2026 Paul R. Decker

# Catch2 for host unit tests. Guide: docs/modules/build-and-dependencies.md.
#
# Catch2 (third_party/Catch2, catchorg/Catch2, Boost Software License 1.0) is a
# host-test dependency only. CMakeLists.txt includes this file for the host
# target alone, so no firmware image compiles or links it, and the firmware
# configure's dependency check leaves it out ("scope": "host-test" in
# dependencies.json). Git records the submodule revision; the inventory checks
# only the local files the build needs.
#
# With BUILD_TESTING (CTest's option, ON by default), Catch2 is built from the
# submodule with the policy of the tests that use it: no exceptions and no RTTI,
# and CATCH_CONFIG_DISABLE_EXCEPTIONS in Catch2's generated
# catch_user_config.hpp, which every unit of Catch2 and of the tests includes.
# A failed REQUIRE therefore ends its test case's process after reporting it
# instead of throwing; CHECK records the failure and goes on. Catch2's targets
# are EXCLUDE_FROM_ALL, so they build only as a test's dependency, and SYSTEM,
# so its headers never trip the owned code's warnings-as-errors. They sit
# below third_party/, which cmake/project_options.cmake leaves alone.
#
# wfg_add_catch2_test(<target> SOURCES <file>... [LIBRARIES <target>...]
#                     [COMPILE_OPTIONS <option>...])
# adds a Catch2 test executable linked with Catch2's main and registers each of
# its TEST_CASEs with ctest as "<target>: <test case name>", labelled catch2.
# Discovery is PRE_TEST: ctest lists the cases when it runs, so a build never
# runs a test binary. The compiler's runtime directory is put first on PATH
# for discovery and for each case, so a MinGW build runs them from any shell
# (Git Bash puts an older libstdc++-6.dll first otherwise). The project's C++
# policy (cmake/project_options.cmake) reaches the target like any other owned
# target. Without BUILD_TESTING the function adds nothing.

set(WFG_CATCH2_DIR "${WFG_ROOT}/third_party/Catch2")

# The release the Catch2 sources declare in catch_version_macros.hpp, as vMAJOR.MINOR.PATCH.
function(wfg_catch2_source_release out_var)
    set(header "${WFG_CATCH2_DIR}/src/catch2/catch_version_macros.hpp")
    set(numbers "")
    foreach(part IN ITEMS MAJOR MINOR PATCH)
        file(STRINGS "${header}" line REGEX "^#define CATCH_VERSION_${part} [0-9]+$")
        if(NOT line)
            message(FATAL_ERROR "${header} defines no CATCH_VERSION_${part}")
        endif()
        string(REGEX REPLACE "^#define CATCH_VERSION_${part} " "" number "${line}")
        list(APPEND numbers "${number}")
    endforeach()
    list(JOIN numbers "." version)
    set(${out_var} "v${version}" PARENT_SCOPE)
endfunction()

# Builds Catch2 from the submodule. A function, so that Catch2's option
# variables stay out of the project's scope.
function(wfg_catch2_add_library)
    if(NOT EXISTS "${WFG_CATCH2_DIR}/CMakeLists.txt")
        message(FATAL_ERROR "third_party/Catch2 is not initialised. Run scripts/init-dependencies.ps1, "
            "or configure with -DBUILD_TESTING=OFF to build the host tools without their tests.")
    endif()
    wfg_catch2_source_release(found)
    # Read by Catch2's option() calls (policy CMP0077): no exceptions in any
    # Catch2 or test unit, and no install rules for documentation or extras.
    set(CATCH_CONFIG_DISABLE_EXCEPTIONS ON)
    set(CATCH_INSTALL_DOCS OFF)
    set(CATCH_INSTALL_EXTRAS OFF)
    add_subdirectory("${WFG_CATCH2_DIR}" "${CMAKE_BINARY_DIR}/third_party/Catch2" EXCLUDE_FROM_ALL SYSTEM)
    foreach(target IN ITEMS Catch2 Catch2WithMain)
        if(MSVC)
            target_compile_options(${target} PRIVATE /GR- /EHs- /EHc-)
            target_compile_definitions(${target} PRIVATE _HAS_EXCEPTIONS=0)
        else()
            target_compile_options(${target} PRIVATE -fno-exceptions -fno-rtti)
        endif()
    endforeach()
    message(STATUS "Catch2 ${found} for host unit tests: ${WFG_CATCH2_DIR}")
endfunction()

if(BUILD_TESTING)
    wfg_catch2_add_library()
    include("${WFG_CATCH2_DIR}/extras/Catch.cmake")
endif()

function(wfg_add_catch2_test target)
    if(NOT BUILD_TESTING)
        return()
    endif()
    cmake_parse_arguments(PARSE_ARGV 1 arg "" "" "SOURCES;LIBRARIES;COMPILE_OPTIONS")
    if(arg_UNPARSED_ARGUMENTS OR NOT arg_SOURCES)
        message(FATAL_ERROR "wfg_add_catch2_test(${target}): SOURCES <file>... [LIBRARIES <target>...] "
            "[COMPILE_OPTIONS <option>...]")
    endif()
    add_executable(${target} ${arg_SOURCES})
    target_link_libraries(${target} PRIVATE ${arg_LIBRARIES} Catch2::Catch2WithMain)
    target_compile_features(${target} PRIVATE cxx_std_23)
    target_compile_options(${target} PRIVATE ${arg_COMPILE_OPTIONS})
    set(runtime_paths "")
    if(MINGW)
        cmake_path(GET CMAKE_CXX_COMPILER PARENT_PATH compiler_dir)
        set(runtime_paths DL_PATHS "${compiler_dir}")
    endif()
    catch_discover_tests(${target}
        TEST_PREFIX "${target}: "
        DISCOVERY_MODE PRE_TEST
        ${runtime_paths}
        PROPERTIES LABELS catch2)
endfunction()
