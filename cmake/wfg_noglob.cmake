# SPDX-License-Identifier: GPL-3.0-only
# SPDX-FileCopyrightText: 2026 Paul R. Decker

# wfg_take_arguments_as_written(<target>): the host tool's main() gets its
# arguments as written. The WinLibs MinGW-w64 runtime expands '*' and '?' in the
# command line before main() runs (its libmingw32 sets _dowildcard to -1), so a
# file name with a wildcard became the files it matched: an output "out-4*.wav"
# named out-48k.wav and overwrote it. The runtime's own CRT_noglob.o defines
# _dowildcard as 0, and an object on the link line comes before that library
# default, so the effect is `int _dowildcard = 0;` in the tool's main unit
# without defining a reserved identifier there, which the clang-tidy profile
# fails (bugprone-reserved-identifier, clang-diagnostic-reserved-identifier).
# channel-sim/CMakeLists.txt does the same for pathsim_chain (979b7a7). Another
# toolchain passes arguments as written already, and this does nothing there.
include_guard(GLOBAL)

function(wfg_take_arguments_as_written target)
    if(NOT MINGW)
        return()
    endif()

    execute_process(COMMAND "${CMAKE_CXX_COMPILER}" -print-file-name=CRT_noglob.o
        OUTPUT_VARIABLE noglob_object OUTPUT_STRIP_TRAILING_WHITESPACE)

    if(NOT IS_ABSOLUTE "${noglob_object}" OR NOT EXISTS "${noglob_object}")
        message(FATAL_ERROR "The MinGW runtime has no CRT_noglob.o ('${noglob_object}'): "
            "${target} would expand wildcards in its arguments.")
    endif()

    cmake_path(NORMAL_PATH noglob_object)
    target_link_libraries(${target} PRIVATE "${noglob_object}")
endfunction()
