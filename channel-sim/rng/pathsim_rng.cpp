// SPDX-License-Identifier: GPL-3.0-only
// SPDX-FileCopyrightText: 2026 Paul R. Decker

// Definitions behind rng/pathsim_rng_shim.hpp (guide: docs/modules/pathsim.md).

#include "rng/pathsim_rng.hpp"

#include "rng/pathsim_rng_shim.hpp"
#include "rng/ucrt_lcg.hpp"

namespace
{
// The one generator. Constant-initialized (state 1, the runtime's unseeded state),
// so it needs no guard and exists before any static constructor runs.
[[nodiscard]] channel_sim::UcrtLcg& generator() noexcept
{
    static channel_sim::UcrtLcg lcg{1U};
    return lcg;
}
} // namespace

namespace PathSim
{
int rand() noexcept
{
    return generator().next();
}
} // namespace PathSim

namespace channel_sim
{
void seed_pathsim_rng(Seed seed) noexcept
{
    generator() = UcrtLcg{seed.value};
}
} // namespace channel_sim
