# SPDX-License-Identifier: GPL-3.0-only
# SPDX-FileCopyrightText: 2026 Paul R. Decker

# cmake/wfg_clang_tools.cmake finds a clang tool at LLVM 19 or later, never an
# older one beside it (ubuntu-24.04's runner image links /usr/bin/clang-tidy to
# clang-tidy-18, which rejects the --exclude-header-filter that
# scripts/clang-tidy.ps1 passes). Run as a script:
#
#   cmake -DWFG_ROOT=<repository> [-DWFG_CLANG_TIDY=<path>] [-DWFG_CLANG_QUERY=<path>] -P tests/test_clang_tools.cmake
#
# The version is read from canned --version texts; the tool cmake itself, which
# reports no LLVM version, stands for a tool too old, found and refused; each
# tool configure found (WFG_CLANG_TIDY, WFG_CLANG_QUERY) must be accepted and
# found again in its own directory. It fails on the first wrong answer.
cmake_minimum_required(VERSION 3.27)
include("${WFG_ROOT}/cmake/wfg_clang_tools.cmake")

function(expect_equal actual expected what)
    if(NOT "${actual}" STREQUAL "${expected}")
        message(FATAL_ERROR "${what}: got '${actual}', expected '${expected}'")
    endif()
endfunction()

# The LLVM major version a --version text reports, and whether it is recent enough.
set(ubuntu_18 "Ubuntu LLVM version 18.1.3\n  Optimized build.\n")
set(llvm_19 "LLVM (http://llvm.org/):\n  LLVM version 19.1.1\n  Optimized build.\n")
set(llvm_20 "Homebrew LLVM version 20.1.8\n  Optimized build.\n")
set(not_llvm "cmake version 3.30.0\n\nCMake suite maintained and supported by Kitware (kitware.com/cmake).\n")
foreach(case IN ITEMS "ubuntu_18;18;FALSE" "llvm_19;19;TRUE" "llvm_20;20;TRUE" "not_llvm;;FALSE")
    list(GET case 0 text)
    list(GET case 1 major)
    list(GET case 2 recent)
    wfg_clang_tool_major("${${text}}" found_major)
    expect_equal("${found_major}" "${major}" "the major version of ${text}")
    wfg_clang_tool_is_recent("${${text}}" found_recent)
    expect_equal("${found_recent}" "${recent}" "whether ${text} is recent enough")
endforeach()

# A tool that runs but reports no LLVM 19 is refused, and the search finds nothing.
set(accepted TRUE)
wfg_clang_tool_validator(accepted "${CMAKE_COMMAND}")
expect_equal("${accepted}" FALSE "the validator on ${CMAKE_COMMAND}")
set(accepted TRUE)
wfg_clang_tool_validator(accepted "${WFG_ROOT}/no-such-clang-tool")
expect_equal("${accepted}" FALSE "the validator on a tool that does not run")
cmake_path(GET CMAKE_COMMAND PARENT_PATH cmake_directory)
cmake_path(GET CMAKE_COMMAND STEM cmake_name)
wfg_find_clang_tool(found_cmake "${cmake_name}" PATHS "${cmake_directory}" NO_DEFAULT_PATH NO_CACHE)
expect_equal("${found_cmake}" "found_cmake-NOTFOUND" "a clang tool named ${cmake_name} in ${cmake_directory}")

# Each tool configure found is LLVM 19 or later, and the search in its directory finds one again.
foreach(variable IN ITEMS WFG_CLANG_TIDY WFG_CLANG_QUERY)
    if(NOT ${variable})
        message(STATUS "${variable} is not set: its own check is skipped")
        continue()
    endif()
    set(accepted TRUE)
    wfg_clang_tool_validator(accepted "${${variable}}")
    expect_equal("${accepted}" TRUE "the validator on ${variable} (${${variable}})")
    cmake_path(GET ${variable} PARENT_PATH tool_directory)
    string(TOLOWER "${variable}" tool)
    string(REPLACE "wfg_clang_" "clang-" tool "${tool}")
    unset(found_tool)
    wfg_find_clang_tool(found_tool "${tool}" PATHS "${tool_directory}" NO_DEFAULT_PATH NO_CACHE)
    if(NOT found_tool)
        message(FATAL_ERROR "${tool} 19 or later is not found in ${tool_directory}, beside ${${variable}}")
    endif()
    cmake_path(GET found_tool STEM found_name)
    if(NOT found_name STREQUAL "${tool}-${WFG_CLANG_TOOLS_MAJOR}" AND NOT found_name STREQUAL "${tool}")
        message(FATAL_ERROR "the search for ${tool} found ${found_tool}")
    endif()
    set(accepted TRUE)
    wfg_clang_tool_validator(accepted "${found_tool}")
    expect_equal("${accepted}" TRUE "the validator on the ${tool} found again (${found_tool})")
    message(STATUS "${variable}: ${${variable}}; found again: ${found_tool}")
endforeach()
message(STATUS "The clang tools are found at LLVM ${WFG_CLANG_TOOLS_MAJOR} or later.")
