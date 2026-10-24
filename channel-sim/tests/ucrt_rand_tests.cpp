// SPDX-License-Identifier: GPL-3.0-only
// SPDX-FileCopyrightText: 2026 Paul R. Decker

// UcrtLcg against this host's C runtime rand()/srand() (guide: docs/modules/pathsim.md).
//
// Built on Windows only, where the runtime is the Universal C Runtime (or msvcrt)
// that the lane's pathsim.exe imports rand and srand from. Every seed pathsim's
// gates use, the extremes, and 100,000 draws each must agree value for value.

#include "rng/ucrt_lcg.hpp"

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

namespace
{
constexpr std::array<std::uint32_t, 12U> seeds{0U, 1U, 2U, 3U, 5U, 31U, 99U, 60000U, 60001U, 0x12345678U, 0x80000000U, 0xFFFFFFFFU};
constexpr int draws_per_seed = 100000;

[[nodiscard]] bool same_stream(std::uint32_t seed) noexcept
{
    channel_sim::UcrtLcg lcg{seed};
    std::srand(seed);

    for (int draw = 0; draw < draws_per_seed; ++draw)
    {
        if (lcg.next() != std::rand())
        {
            return false;
        }
    }

    return true;
}
} // namespace

int main()
{
    int failures = 0;

    if (RAND_MAX != channel_sim::ucrt_rand_max)
    {
        std::fputs("FAIL: this runtime's RAND_MAX is not 0x7FFF\n", stdout);
        ++failures;
    }

    for (const std::uint32_t seed : seeds)
    {
        if (!same_stream(seed))
        {
            std::fputs("FAIL: UcrtLcg differs from the runtime's rand() for a seed\n", stdout);
            ++failures;
        }
    }

    std::fputs(failures == 0 ? "UCRT rand tests: PASS\n" : "UCRT rand tests: FAIL\n", stdout);
    return failures == 0 ? 0 : 1;
}
