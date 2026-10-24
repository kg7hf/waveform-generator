// SPDX-License-Identifier: GPL-3.0-only
// SPDX-FileCopyrightText: 2026 Paul R. Decker

// Box-Muller on PCG32 draws (signal_lab/det_gaussian.hpp).
// Guide: docs/modules/signal-lab-watterson.md, "Tap processes".

#include "signal_lab/det_gaussian.hpp"

#include "signal_lab/convert.hpp"

#include <cmath>

namespace signal_lab::det
{
namespace
{

// 2^-32: one step of a 32-bit draw on [0, 1).
constexpr double draw_step = 0x1p-32;
// Pcg32::uniform() keeps the top 24 bits of a draw: shift out 8, scale by 2^-24.
constexpr std::uint32_t uniform_discarded_bits = 8U;
constexpr double uniform_step = 0x1p-24;
// Box-Muller: r^2 = -2 ln u1 for a pair of unit-variance components.
constexpr double radius_log_scale = -2.0;

} // namespace

GaussianPair gaussian_pair(std::uint32_t radius_draw, std::uint32_t angle_draw) noexcept
{
    const double u1 = (as_double(radius_draw) + 1.0) * draw_step;
    const double u2 = as_double(angle_draw >> uniform_discarded_bits) * uniform_step;
    const double radius = std::sqrt(radius_log_scale * log(u1));
    const double angle = two_pi * u2;
    return GaussianPair{radius * cos(angle), radius * sin(angle)};
}

GaussianPair draw_gaussian_pair(Pcg32& generator) noexcept
{
    const std::uint32_t radius_draw = generator.next();
    const std::uint32_t angle_draw = generator.next();
    return gaussian_pair(radius_draw, angle_draw);
}

} // namespace signal_lab::det
