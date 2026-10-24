// SPDX-License-Identifier: GPL-3.0-only
// SPDX-FileCopyrightText: 2026 Paul R. Decker

// The random stream pathsim draws from in the chain build (guide: docs/modules/pathsim.md).
//
// pathsim calls rand() for every random draw (Rayleigh taps and AWGN). In the chain
// build, rng/pathsim_rng_shim.hpp is force-included into pathsim's sources and makes
// those calls resolve to PathSim::rand(), which steps one process-wide UcrtLcg.
// seed_pathsim_rng() is the srand(--seed) of the patched main.cpp.
//
// Ownership: one job at a time owns the generator. The chain driver seeds it
// immediately before pathsim's init() and nothing else may draw from it until the
// job ends; a second concurrent job would interleave the streams.

#pragma once

#include "core/units.hpp"

namespace channel_sim
{

// srand(seed.value) for pathsim's draws.
void seed_pathsim_rng(Seed seed) noexcept;

} // namespace channel_sim
