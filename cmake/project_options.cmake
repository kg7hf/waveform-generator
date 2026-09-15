# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 Paul R. Decker

# One C++ policy for host tools, portable libraries, tests and owned firmware.
# Vendor C sources retain the upstream compiler policy.
function(wfg_apply_project_options target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /WX /permissive- /GR- /EHs- /EHc-)
        target_compile_definitions(${target} PRIVATE _HAS_EXCEPTIONS=0)
    else()
        target_compile_options(${target} PRIVATE
            $<$<COMPILE_LANGUAGE:CXX>:-Wall;-Wextra;-Wpedantic;-Werror;-fno-exceptions;-fno-rtti>)
    endif()
endfunction()

function(wfg_apply_directory_options directory)
    get_property(targets DIRECTORY "${directory}" PROPERTY BUILDSYSTEM_TARGETS)
    foreach(target IN LISTS targets)
        get_target_property(type ${target} TYPE)
        if(type MATCHES "^(EXECUTABLE|STATIC_LIBRARY|OBJECT_LIBRARY|SHARED_LIBRARY)$")
            wfg_apply_project_options(${target})
        endif()
    endforeach()
    get_property(children DIRECTORY "${directory}" PROPERTY SUBDIRECTORIES)
    foreach(child IN LISTS children)
        if(NOT child MATCHES "/third_party/")
            wfg_apply_directory_options("${child}")
        endif()
    endforeach()
endfunction()
