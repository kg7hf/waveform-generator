cmake_minimum_required(VERSION 3.27)

set(WFG_TARGET "host" CACHE STRING "Build target: host or rt1170")
set_property(CACHE WFG_TARGET PROPERTY STRINGS host rt1170)

if(WFG_TARGET STREQUAL "rt1170")
    project(M110WaveformGenerator VERSION 0.1.0 LANGUAGES C CXX ASM)
else()
    project(M110WaveformGenerator VERSION 0.1.0 LANGUAGES CXX)
endif()

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

file(REAL_PATH "${CMAKE_CURRENT_SOURCE_DIR}" WFG_ROOT)
file(REAL_PATH "${CMAKE_CURRENT_BINARY_DIR}" WFG_BUILD_ROOT)

# Ask CMake's File API to retain the configure inputs and codemodel needed by
# scripts/verify-standalone.py in the same configure invocation. Replies are
# generated in the local build directory and are never source inputs.
cmake_file_api(
    QUERY
    API_VERSION 1
    CODEMODEL 2
    CMAKEFILES 1)

foreach(identity_file IN ITEMS
        "${WFG_ROOT}/dependencies.lock.json"
        "${WFG_ROOT}/docs/source-imports.json")
    if(NOT EXISTS "${identity_file}")
        message(FATAL_ERROR "Required local identity input is missing: ${identity_file}")
    endif()
endforeach()

file(SHA256 "${WFG_ROOT}/dependencies.lock.json" WFG_DEPENDENCY_LOCK_SHA256)
file(SHA256 "${WFG_ROOT}/docs/source-imports.json" WFG_SOURCE_IMPORTS_SHA256)

# Build identity is derived from actual generator-owned bytes and remains valid
# after this directory is extracted without Git metadata. Third-party bytes are
# excluded here because dependencies.lock.json contains their complete per-file
# hashes; that lock file itself is included in this manifest.
file(GLOB_RECURSE WFG_SOURCE_IDENTITY_FILES
    LIST_DIRECTORIES false RELATIVE "${WFG_ROOT}" "${WFG_ROOT}/*")
list(FILTER WFG_SOURCE_IDENTITY_FILES EXCLUDE REGEX
    "^(artifacts|build|third_party|\\.venv|\\.git)(/|$)|(^|/)(__pycache__|\\.pytest_cache)(/|$)")
list(SORT WFG_SOURCE_IDENTITY_FILES)
set(WFG_SOURCE_MANIFEST "{\n  \"schema_version\": \"wfg-source-manifest/1\",\n  \"files\": [\n")
set(WFG_SOURCE_MANIFEST_SEPARATOR "")
foreach(relative_path IN LISTS WFG_SOURCE_IDENTITY_FILES)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
        "${WFG_ROOT}/${relative_path}")
    file(SIZE "${WFG_ROOT}/${relative_path}" source_size)
    file(SHA256 "${WFG_ROOT}/${relative_path}" source_sha256)
    string(REPLACE "\\" "\\\\" escaped_path "${relative_path}")
    string(REPLACE "\"" "\\\"" escaped_path "${escaped_path}")
    string(APPEND WFG_SOURCE_MANIFEST
        "${WFG_SOURCE_MANIFEST_SEPARATOR}    {\"path\": \"${escaped_path}\", \"size_bytes\": ${source_size}, \"sha256\": \"${source_sha256}\"}")
    set(WFG_SOURCE_MANIFEST_SEPARATOR ",\n")
endforeach()
string(APPEND WFG_SOURCE_MANIFEST "\n  ]\n}\n")
set(WFG_SOURCE_MANIFEST_PATH "${CMAKE_BINARY_DIR}/source-manifest.json")
file(WRITE "${WFG_SOURCE_MANIFEST_PATH}" "${WFG_SOURCE_MANIFEST}")
file(SHA256 "${WFG_SOURCE_MANIFEST_PATH}" WFG_SOURCE_MANIFEST_SHA256)

set(WFG_BUILD_ID
    "wfg-0.1.0,source=${WFG_SOURCE_MANIFEST_SHA256},deps=${WFG_DEPENDENCY_LOCK_SHA256},imports=${WFG_SOURCE_IMPORTS_SHA256}")

if(WFG_TARGET STREQUAL "host")
    include(CTest)
    enable_testing()
    add_subdirectory(signal-lab)
    add_subdirectory(host/capture)
    add_subdirectory(host/render)
    add_subdirectory(tests)
elseif(WFG_TARGET STREQUAL "rt1170")
    add_subdirectory(rt1170)
else()
    message(FATAL_ERROR "Unsupported WFG_TARGET '${WFG_TARGET}'")
endif()
