// SPDX-License-Identifier: GPL-3.0-only
// SPDX-FileCopyrightText: 2026 Paul R. Decker

// RNG shim, force-included (-include) into pathsim's library sources in the chain
// build only (guide: docs/modules/pathsim.md). pathsim_ref, the reference CLI, is
// built without it. This header is the chain's single sanctioned macro boundary
// into pathsim's sources. It edits no pathsim file, but it does modify pathsim's
// compiled behaviour (which generator rand() is, and RAND_MAX on newlib), so its
// dated GPLv3 s5a notice is patches/pathsim-wfg-embedded.notice.md, section 1.
//
// The shim fixes which numbers are drawn, not which variable receives each one:
// pathsim's v.set(rsample(), rsample()) leaves the order of its two draws to the
// compiler, and x86-64 and Arm GCC differ. patches/pathsim-wfg-embedded.patch
// sequences both sites in the lane binary's order.
//
// How it maps rand(): pathsim draws with unqualified rand() calls inside namespace
// PathSim (Path.cpp, NoiseGen.cpp). Declaring PathSim::rand() here, before any
// pathsim code, makes ordinary name lookup find it first, so those calls step the
// UCRT-compatible generator of rng/pathsim_rng.cpp instead of the C runtime's. No
// macro renames rand. srand is pathsim main.cpp's, which the chain does not compile;
// the driver calls seed_pathsim_rng() in its place. The build checks that pathsim's
// objects reference PathSim::rand() and not the runtime's rand/srand
// (cmake/check_pathsim_rng.cmake).
//
// RAND_MAX: pathsim scales by double(RAND_MAX), which must be the generator's 0x7FFF.
// The Microsoft runtimes already define that; other C libraries (glibc, newlib) define
// 0x7FFFFFFF, so for pathsim's sources only the value is replaced there. That
// redefinition is the shim's one macro.

#pragma once

#include "rng/ucrt_lcg.hpp"

#include <cstdlib>

#if RAND_MAX != 0x7FFF
#undef RAND_MAX
#define RAND_MAX 0x7FFF
#endif

static_assert(RAND_MAX == channel_sim::ucrt_rand_max, "pathsim must scale rand() by the UCRT generator's maximum");

namespace PathSim
{
// The next value of the UCRT-compatible stream, 0..0x7FFF (rng/pathsim_rng.cpp).
[[nodiscard]] int rand() noexcept;
} // namespace PathSim
