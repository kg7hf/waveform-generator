// SPDX-License-Identifier: GPL-3.0-only
// SPDX-FileCopyrightText: 2026 Paul R. Decker

// The Microsoft C runtime's rand()/srand() generator (guide: docs/modules/pathsim.md).
//
// The lane's pathsim.exe imports rand and srand from the Universal C Runtime
// (api-ms-win-crt-utility-l1-1-0.dll). UCRT, like msvcrt before it, keeps a 32-bit
// state per thread: srand(seed) stores seed; rand() steps
//     state = state * 214013 + 2531011   (mod 2^32)
// and returns bits 16..30 of the new state, 0..RAND_MAX with RAND_MAX = 0x7FFF.
// Without srand the state is 1. pathsim always calls srand (--seed, default 1).
//
// UcrtLcg reproduces that stream on any platform, so the RT1170 build (newlib,
// whose rand differs) draws the same numbers as the lane's binary. The same
// realization also needs each pair of draws assigned in the lane's order, which
// patches/pathsim-wfg-embedded.patch fixes for every compiler (guide, "The RNG
// shim"). channel-sim/tests/ucrt_rand_tests.cpp compares UcrtLcg with the host
// runtime's rand.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace channel_sim
{

inline constexpr std::uint32_t ucrt_lcg_multiplier = 214013U;
inline constexpr std::uint32_t ucrt_lcg_increment = 2531011U;
inline constexpr unsigned ucrt_lcg_output_shift = 16U;
inline constexpr std::uint32_t ucrt_rand_mask = 0x7FFFU;
// RAND_MAX of the Microsoft runtimes; pathsim divides rand() by RAND_MAX.
inline constexpr int ucrt_rand_max = 0x7FFF;

class UcrtLcg
{
public:
    // srand(seed).
    constexpr explicit UcrtLcg(std::uint32_t seed) noexcept : state_{seed} {}

    // rand(): unsigned arithmetic wraps modulo 2^32, as the runtime's does.
    [[nodiscard]] constexpr int next() noexcept
    {
        state_ = state_ * ucrt_lcg_multiplier + ucrt_lcg_increment;
        return static_cast<int>((state_ >> ucrt_lcg_output_shift) & ucrt_rand_mask);
    }

private:
    std::uint32_t state_;
};

namespace detail
{
// The first ten rand() values after srand(1) (or no srand) in every Microsoft C
// runtime: the generator's published fingerprint.
inline constexpr std::array<int, 10U> ucrt_seed1_prefix{41, 18467, 6334, 26500, 19169, 15724, 11478, 29358, 26962, 24464};

[[nodiscard]] constexpr bool matches_seed1_prefix() noexcept
{
    UcrtLcg lcg{1U};

    for (const int expected : ucrt_seed1_prefix)
    {
        if (lcg.next() != expected)
        {
            return false;
        }
    }

    return true;
}
} // namespace detail

static_assert(detail::matches_seed1_prefix(), "UcrtLcg must reproduce the Microsoft rand() sequence for seed 1");

} // namespace channel_sim
