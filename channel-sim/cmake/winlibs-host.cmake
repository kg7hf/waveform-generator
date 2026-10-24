# SPDX-License-Identifier: GPL-3.0-only
# SPDX-FileCopyrightText: 2026 Paul R. Decker
#
# Host toolchain for the channel-sim build (docs/modules/pathsim.md).
#
# The HF lane's shipped pathsim.exe (sha256 77461211...) was built with WinLibs
# GCC 14.2.0 (MinGW-W64 x86_64-ucrt-posix-seh, r2). Bit-exact parity with it is
# only claimed for that compiler, so this file selects it when it is installed.
# Otherwise CMake's normal compiler search runs (the repo's host compiler) and
# the configure step warns that parity with the shipped binary is unproven.
#
# Override with -DWFG_WINLIBS_BIN=<dir containing g++.exe>.

set(WFG_WINLIBS_BIN
    "$ENV{LOCALAPPDATA}/Microsoft/WinGet/Packages/BrechtSanders.WinLibs.POSIX.UCRT.LLVM_Microsoft.Winget.Source_8wekyb3d8bbwe/mingw64/bin"
    CACHE PATH "WinLibs mingw64/bin holding the GCC that built the lane's pathsim.exe")

if(EXISTS "${WFG_WINLIBS_BIN}/g++.exe")
    set(CMAKE_CXX_COMPILER "${WFG_WINLIBS_BIN}/g++.exe")
    # Ninja needs its own build program; select mingw32-make only for the
    # matching CMake generator.
    if(CMAKE_GENERATOR STREQUAL "MinGW Makefiles"
       AND EXISTS "${WFG_WINLIBS_BIN}/mingw32-make.exe"
       AND NOT CMAKE_MAKE_PROGRAM)
        set(CMAKE_MAKE_PROGRAM "${WFG_WINLIBS_BIN}/mingw32-make.exe" CACHE FILEPATH "Make program from the WinLibs toolchain")
    endif()
endif()
