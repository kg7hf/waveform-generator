# SPDX-License-Identifier: GPL-3.0-only
# SPDX-FileCopyrightText: 2026 Paul R. Decker
#
# Post-build check of pathsim_lib (docs/modules/pathsim.md, "RNG shim").
#
# The RNG shim works by name lookup: pathsim's unqualified rand() calls inside
# namespace PathSim must bind to PathSim::rand(). If a future pathsim called
# ::rand() or std::rand(), or a source escaped the -include, its draws would
# silently come from the C runtime again. This script fails the build unless the
# library references PathSim::rand() (_ZN7PathSim4randEv) and references neither
# the runtime's rand nor srand.
#
#   cmake -DNM=<nm> -DLIBRARY=<libpathsim_lib.a> -P check_pathsim_rng.cmake

if(NOT NM OR NOT LIBRARY)
    message(FATAL_ERROR "check_pathsim_rng: pass -DNM=<nm> -DLIBRARY=<archive>")
endif()

execute_process(COMMAND "${NM}" --undefined-only "${LIBRARY}"
    RESULT_VARIABLE result
    OUTPUT_VARIABLE undefined
    ERROR_VARIABLE error)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "check_pathsim_rng: '${NM} --undefined-only ${LIBRARY}' failed: ${error}")
endif()

string(REPLACE "\r" "" undefined "${undefined}")
string(REPLACE "\n" ";" lines "${undefined}")
set(shim_references 0)
foreach(line IN LISTS lines)
    string(STRIP "${line}" line)
    if(line MATCHES "^U (_?rand|_?srand)$")
        message(FATAL_ERROR "check_pathsim_rng: ${LIBRARY} references the C runtime's ${CMAKE_MATCH_1}: "
            "a pathsim draw escaped the RNG shim (rng/pathsim_rng_shim.hpp).")
    endif()
    if(line MATCHES "^U _?_ZN7PathSim4randEv$")
        math(EXPR shim_references "${shim_references} + 1")
    endif()
endforeach()

if(shim_references EQUAL 0)
    message(FATAL_ERROR "check_pathsim_rng: ${LIBRARY} does not reference PathSim::rand(); the RNG shim was not applied.")
endif()

message(STATUS "check_pathsim_rng: ${shim_references} object(s) draw from PathSim::rand(); none from the C runtime's rand/srand")
