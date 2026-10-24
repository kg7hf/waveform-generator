// SPDX-License-Identifier: GPL-3.0-only
// SPDX-FileCopyrightText: 2026 Paul R. Decker

// The Kaiser window's I0 series (signal_lab/kaiser.hpp).
// Guide: docs/modules/signal-lab-watterson.md, "Hilbert filter".

#include "signal_lab/kaiser.hpp"

#include <cmath>

namespace signal_lab::kaiser
{

double bessel_i0(double x) noexcept
{
    const double quarter_square = 0.25 * x * x; // (x / 2)^2
    double term = 1.0;
    double sum = 1.0;

    for (std::uint32_t k = 1U; k < series_terms; ++k)
    {
        const double order = as_double(k);
        term *= quarter_square / (order * order);
        sum += term;
    }

    return sum;
}

double window(double beta, double position) noexcept
{
    const double inside = 1.0 - position * position;
    const double argument = inside > 0.0 ? beta * std::sqrt(inside) : 0.0;
    return bessel_i0(argument) / bessel_i0(beta);
}

} // namespace signal_lab::kaiser
