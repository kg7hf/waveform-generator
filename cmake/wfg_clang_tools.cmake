# SPDX-License-Identifier: GPL-3.0-only
# SPDX-FileCopyrightText: 2026 Paul R. Decker

# wfg_find_clang_tool(<variable> <tool> [find_program options]): the clang tool
# the host tests run (clang-tidy for scripts/clang-tidy.ps1, clang-query for
# scripts/check-c-status-truth.py) at LLVM 19 or later, into the cache entry
# <variable>, or <variable>-NOTFOUND. The script and the repository's
# .clang-tidy need clang-tidy 19 (--exclude-header-filter and
# ExcludeHeaderFilterRegex came in 19), and an older default can sit beside the
# versioned tool: ubuntu-24.04's runner image links /usr/bin/clang-tidy to
# clang-tidy-18, and find_program tries each name in every directory before the
# next name. So <tool>-19 is tried first, then <tool>, and each candidate must
# run and report LLVM 19 or later in its --version text, or the search goes on.
# tests/test_clang_tools.cmake holds it to that.
include_guard(GLOBAL)

set(WFG_CLANG_TOOLS_MAJOR 19)

# The LLVM major version a tool's --version text reports, or "" when it reports none.
function(wfg_clang_tool_major text out_variable)
    if(text MATCHES "LLVM version ([0-9]+)\\.")
        set(${out_variable} "${CMAKE_MATCH_1}" PARENT_SCOPE)
    else()
        set(${out_variable} "" PARENT_SCOPE)
    endif()
endfunction()

# TRUE when the --version text reports LLVM 19 or later, else FALSE.
function(wfg_clang_tool_is_recent text out_variable)
    wfg_clang_tool_major("${text}" major)
    if(major MATCHES "^[0-9]+$" AND NOT major LESS WFG_CLANG_TOOLS_MAJOR)
        set(${out_variable} TRUE PARENT_SCOPE)
    else()
        set(${out_variable} FALSE PARENT_SCOPE)
    endif()
endfunction()

# find_program's VALIDATOR: refuses a candidate whose --version text reports no LLVM 19 or later (one that does
# not run prints none).
function(wfg_clang_tool_validator result_variable candidate)
    execute_process(COMMAND "${candidate}" --version OUTPUT_VARIABLE text ERROR_QUIET TIMEOUT 60)
    wfg_clang_tool_is_recent("${text}" recent)
    if(NOT recent)
        set(${result_variable} FALSE PARENT_SCOPE)
    endif()
endfunction()

macro(wfg_find_clang_tool variable tool)
    find_program(${variable} NAMES ${tool}-${WFG_CLANG_TOOLS_MAJOR} ${tool} VALIDATOR wfg_clang_tool_validator ${ARGN})
endmacro()
