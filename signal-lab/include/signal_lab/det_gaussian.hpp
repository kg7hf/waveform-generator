// SPDX-License-Identifier: GPL-3.0-only
// SPDX-FileCopyrightText: 2026 Paul R. Decker

#pragma once

// Exact Gaussian pairs from two PCG32 draws: Box-Muller (Box and Muller, 1958).
// Guide: docs/modules/signal-lab-watterson.md, "Tap processes".
//
// One primitive for the Watterson tap processes and the AWGN excitation. It is a
// pure function of the two draws, so a test can inject any draw:
//   u1 = (double(radius_draw) + 1) x 2^-32       in [2^-32, 1]: widened BEFORE the + 1,
//                                                 so 0xFFFFFFFF gives u1 = 1, not 0
//   u2 = double(angle_draw >> 8) x 2^-24         in [0, 1), the 24 bits of Pcg32::uniform()
//   r  = sqrt(-2 ln u1) <= 6.660                 first = r cos(2 pi u2), second = r sin(2 pi u2)
// Each component is N(0, 1): unit variance per real sample. Exactly two draws per
// pair (no rejection loop), taken in two statements, radius first, so the draw
// order is the same on every compiler (pathsim's draw-order lesson).

#include "signal_lab/det_math.hpp"

#include <cstdint>

namespace signal_lab::det
{

struct GaussianPair
{
    double first{};
    double second{};
};

[[nodiscard]] GaussianPair gaussian_pair(std::uint32_t radius_draw, std::uint32_t angle_draw) noexcept;

// Two draws from `generator`, radius first, then gaussian_pair().
[[nodiscard]] GaussianPair draw_gaussian_pair(Pcg32& generator) noexcept;

} // namespace signal_lab::det
