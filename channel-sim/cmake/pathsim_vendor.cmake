# SPDX-License-Identifier: GPL-3.0-only
# SPDX-FileCopyrightText: 2026 Paul R. Decker
#
# Prepares the patched build-tree copies of pathsim (docs/modules/pathsim.md).
#
# third_party/pathsim stays pristine. Configure exports the checked-out
# submodule commit's blobs (never the working tree, so local edits or line-ending
# conversion cannot leak in), applies the patches outside any repository, and
# syncs the results into the build tree. A patch that does not apply stops the
# configure. Two copies come out:
#   <build>/pathsim-src        rev-6 (the lane's patch): pathsim_ref, the reference CLI
#   <build>/pathsim-chain-src  rev-6 + pathsim-wfg-embedded.patch: pathsim_lib, the chain
# Each copy carries its dated modification notices (GPLv3 section 5a).
#
# In:  WFG_ROOT (repository root).
# Out: PATHSIM_PATCHED_DIR (rev-6 copy) and PATHSIM_CHAIN_DIR (chain copy);
#      PATHSIM_LIBRARY_SOURCES (chain copy), PATHSIM_CLI_SOURCES and
#      PATHSIM_REFERENCE_SOURCES (rev-6 copy, all CLI sources in upstream's order),
#      as absolute paths; PATHSIM_COMMIT, PATHSIM_PATCH and PATHSIM_CHAIN_PATCH.

# The first patch is the HF lane's rev-6. Git records the submodule revision;
# the patch must apply to that checkout's committed sources.
set(PATHSIM_PATCH_NAME pathsim-b5fa9ae-gcc14-cli-hilbert-onset-seed-paths-truth)
# waveform-generator's own chain-build patch, applied after rev-6 (its header
# and notice say what and why).
set(PATHSIM_CHAIN_PATCH_NAME pathsim-wfg-embedded)

set(PATHSIM_SUBMODULE_DIR "${WFG_ROOT}/third_party/pathsim")
set(PATHSIM_PATCH "${WFG_ROOT}/patches/${PATHSIM_PATCH_NAME}.patch")
set(PATHSIM_NOTICE "${WFG_ROOT}/patches/${PATHSIM_PATCH_NAME}.notice.md")
set(PATHSIM_CHAIN_PATCH "${WFG_ROOT}/patches/${PATHSIM_CHAIN_PATCH_NAME}.patch")
set(PATHSIM_CHAIN_NOTICE "${WFG_ROOT}/patches/${PATHSIM_CHAIN_PATCH_NAME}.notice.md")
set(PATHSIM_STAGE_DIR "${CMAKE_BINARY_DIR}/pathsim-stage")
set(PATHSIM_CHAIN_STAGE_DIR "${CMAKE_BINARY_DIR}/pathsim-chain-stage")
set(PATHSIM_PATCHED_DIR "${CMAKE_BINARY_DIR}/pathsim-src")
set(PATHSIM_CHAIN_DIR "${CMAKE_BINARY_DIR}/pathsim-chain-src")

set(pathsim_library_files Delay.cpp GaussFIR.cpp NoiseGen.cpp Path.cpp PathSimParams.cpp PathSimProcessor.cpp)
set(pathsim_header_files cmplx.h Delay.h FilterTables.h GaussFIR.h NoiseGen.h Path.h PathSimParams.h PathSimProcessor.h)
# The host CLI only: main.cpp, AudioFile.h (GPLv3+) and cxxopts.h (MIT).
set(pathsim_cli_files main.cpp AudioFile.h cxxopts.h)
# Licence and credit files travel with the modified copy.
set(pathsim_notice_files LICENSE COPYING README.txt README.orig.txt)
set(pathsim_files ${pathsim_library_files} ${pathsim_header_files} ${pathsim_cli_files} ${pathsim_notice_files})

find_package(Git REQUIRED)

# Runs git with the given arguments; stops the configure on failure.
function(pathsim_git out_var)
    execute_process(COMMAND "${GIT_EXECUTABLE}" ${ARGN}
        RESULT_VARIABLE result
        OUTPUT_VARIABLE output
        ERROR_VARIABLE error
        OUTPUT_STRIP_TRAILING_WHITESPACE)
    if(NOT result EQUAL 0)
        list(JOIN ARGN " " command)
        message(FATAL_ERROR "pathsim: 'git ${command}' failed: ${error}")
    endif()
    set(${out_var} "${output}" PARENT_SCOPE)
endfunction()

# Applies patch to the stage directory: every touched file must be an exported
# pathsim file. Git apply checks the context. Sets count_var to the number of
# files patched.
function(pathsim_apply_patch stage patch count_var)
    cmake_path(GET patch STEM LAST_ONLY patch_name)
    file(STRINGS "${patch}" headers ENCODING UTF-8 REGEX "^diff --git a/")
    set(patched_files "")
    foreach(line IN LISTS headers)
        string(STRIP "${line}" line)
        if(line MATCHES "^diff --git a/([^ ]+) b/([^ ]+)$")
            set(current "${CMAKE_MATCH_2}")
            if(NOT current IN_LIST pathsim_files)
                message(FATAL_ERROR "pathsim: ${patch_name}.patch touches ${current}, which this build does not export.")
            endif()
            list(APPEND patched_files "${current}")
        endif()
    endforeach()
    list(LENGTH patched_files count)
    if(count EQUAL 0)
        message(FATAL_ERROR "pathsim: no 'diff --git' sections found in ${patch}.")
    endif()
    # Outside any repository: the ceiling stops git from finding the enclosing
    # checkout, so patch paths resolve against the stage directory.
    execute_process(
        COMMAND "${CMAKE_COMMAND}" -E env "GIT_CEILING_DIRECTORIES=${CMAKE_BINARY_DIR}"
                "${GIT_EXECUTABLE}" apply --verbose --whitespace=nowarn "${patch}"
        WORKING_DIRECTORY "${stage}"
        RESULT_VARIABLE result
        OUTPUT_VARIABLE output
        ERROR_VARIABLE error)
    if(NOT result EQUAL 0)
        message(FATAL_ERROR "pathsim: ${patch_name}.patch does not apply cleanly to ${PATHSIM_COMMIT}:\n${output}${error}")
    endif()
    message(VERBOSE "pathsim: git apply ${patch_name}.patch:\n${output}${error}")
    set(${count_var} ${count} PARENT_SCOPE)
endfunction()

# Syncs stage into destination; unchanged files keep their timestamps.
function(pathsim_sync stage destination)
    file(MAKE_DIRECTORY "${destination}")
    file(GLOB existing RELATIVE "${destination}" "${destination}/*")
    foreach(name IN LISTS existing)
        if(NOT EXISTS "${stage}/${name}")
            file(REMOVE_RECURSE "${destination}/${name}")
        endif()
    endforeach()
    file(GLOB staged RELATIVE "${stage}" "${stage}/*")
    foreach(name IN LISTS staged)
        file(COPY_FILE "${stage}/${name}" "${destination}/${name}" ONLY_IF_DIFFERENT)
    endforeach()
    file(REMOVE_RECURSE "${stage}")
endfunction()

# 1. The submodule is initialised; Git supplies its checked-out revision.
if(NOT EXISTS "${PATHSIM_SUBMODULE_DIR}/.git")
    message(FATAL_ERROR "pathsim: ${PATHSIM_SUBMODULE_DIR} is not initialised; run "
        "'git submodule update --init third_party/pathsim'.")
endif()
pathsim_git(pathsim_head -C "${PATHSIM_SUBMODULE_DIR}" rev-parse HEAD)
set(PATHSIM_COMMIT "${pathsim_head}")
pathsim_git(pathsim_dirty -C "${PATHSIM_SUBMODULE_DIR}" status --porcelain)
if(NOT pathsim_dirty STREQUAL "")
    message(WARNING "pathsim: third_party/pathsim has local changes, which this build ignores: it exports the "
        "committed blobs of ${PATHSIM_COMMIT}. Keep the submodule pristine; changes belong in patches/.\n${pathsim_dirty}")
endif()
pathsim_git(pathsim_head_file -C "${PATHSIM_SUBMODULE_DIR}" rev-parse --path-format=absolute --git-path HEAD)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${pathsim_head_file}" "${PATHSIM_PATCH}" "${PATHSIM_NOTICE}"
    "${PATHSIM_CHAIN_PATCH}" "${PATHSIM_CHAIN_NOTICE}")

# 2. Export the committed blobs of the checked-out revision into a fresh stage.
file(REMOVE_RECURSE "${PATHSIM_STAGE_DIR}" "${PATHSIM_CHAIN_STAGE_DIR}")
file(MAKE_DIRECTORY "${PATHSIM_STAGE_DIR}")
foreach(name IN LISTS pathsim_files)
    execute_process(COMMAND "${GIT_EXECUTABLE}" -C "${PATHSIM_SUBMODULE_DIR}" cat-file blob "${PATHSIM_COMMIT}:${name}"
        OUTPUT_FILE "${PATHSIM_STAGE_DIR}/${name}"
        RESULT_VARIABLE result
        ERROR_VARIABLE error)
    if(NOT result EQUAL 0)
        message(FATAL_ERROR "pathsim: cannot export ${name} from ${PATHSIM_COMMIT}: ${error}")
    endif()
endforeach()

# 3. rev-6, with its dated notice (GPLv3 s5a): the reference copy.
pathsim_apply_patch("${PATHSIM_STAGE_DIR}" "${PATHSIM_PATCH}" pathsim_patched_count)
file(COPY_FILE "${PATHSIM_NOTICE}" "${PATHSIM_STAGE_DIR}/MODIFIED-BY-WAVEFORM-GENERATOR.md")

# 4. The chain copy: rev-6 plus pathsim-wfg-embedded.patch, with both notices.
file(COPY "${PATHSIM_STAGE_DIR}/" DESTINATION "${PATHSIM_CHAIN_STAGE_DIR}")
pathsim_apply_patch("${PATHSIM_CHAIN_STAGE_DIR}" "${PATHSIM_CHAIN_PATCH}" pathsim_chain_patched_count)
file(COPY_FILE "${PATHSIM_CHAIN_NOTICE}" "${PATHSIM_CHAIN_STAGE_DIR}/MODIFIED-BY-WAVEFORM-GENERATOR-CHAIN.md")

# 5. No statement of the chain copy may take two draws: C++ leaves the order of
#    a call's arguments unspecified, and compilers differ (x86-64 and Arm GCC
#    do). The gates check the compiled order (channel-sim/gates/check_draw_order.py).
foreach(name IN LISTS pathsim_library_files)
    file(READ "${PATHSIM_CHAIN_STAGE_DIR}/${name}" pathsim_source)
    string(REGEX MATCH "(rsample|rand)\\(\\)[^;]*(rsample|rand)\\(\\)" pathsim_draw_pair "${pathsim_source}")
    if(pathsim_draw_pair)
        message(FATAL_ERROR "pathsim: ${name} takes two random draws in one statement ('${pathsim_draw_pair}'); their "
            "order is unspecified. Sequence them in ${PATHSIM_CHAIN_PATCH_NAME}.patch.")
    endif()
endforeach()

# 6. Sync both copies into the build tree.
pathsim_sync("${PATHSIM_STAGE_DIR}" "${PATHSIM_PATCHED_DIR}")
pathsim_sync("${PATHSIM_CHAIN_STAGE_DIR}" "${PATHSIM_CHAIN_DIR}")

list(TRANSFORM pathsim_library_files PREPEND "${PATHSIM_CHAIN_DIR}/" OUTPUT_VARIABLE PATHSIM_LIBRARY_SOURCES)
set(PATHSIM_CLI_SOURCES "${PATHSIM_PATCHED_DIR}/main.cpp")
# Upstream's own CMakeLists.txt order (main.cpp between GaussFIR.cpp and
# NoiseGen.cpp), which fixes the lane binary's object and link order.
set(pathsim_upstream_order Delay.cpp GaussFIR.cpp main.cpp NoiseGen.cpp Path.cpp PathSimParams.cpp PathSimProcessor.cpp)
list(TRANSFORM pathsim_upstream_order PREPEND "${PATHSIM_PATCHED_DIR}/" OUTPUT_VARIABLE PATHSIM_REFERENCE_SOURCES)

string(SUBSTRING "${PATHSIM_COMMIT}" 0 7 pathsim_short)
message(STATUS "pathsim: ${pathsim_short} + ${PATHSIM_PATCH_NAME}.patch -> ${PATHSIM_PATCHED_DIR} "
    "(${pathsim_patched_count} files patched)")
message(STATUS "pathsim: ... + ${PATHSIM_CHAIN_PATCH_NAME}.patch -> ${PATHSIM_CHAIN_DIR} "
    "(${pathsim_chain_patched_count} files patched; no unsequenced draw pair)")
