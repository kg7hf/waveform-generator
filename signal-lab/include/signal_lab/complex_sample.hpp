// SPDX-License-Identifier: GPL-3.0-only
// SPDX-FileCopyrightText: 2026 Paul R. Decker

#pragma once

// Complex values of the Watterson stage, with their arithmetic written out.
// Guide: docs/modules/signal-lab-watterson.md, "Determinism".
//
// std::complex is not used: its operator* follows C99 Annex G (an inf/NaN
// recovery call on GCC), and the stage's bit-exact definition needs the plain
// four products in a stated order. Each operator below is that order.

namespace signal_lab
{

struct ComplexFloat
{
    float real{};
    float imag{};
};

struct ComplexDouble
{
    double real{};
    double imag{};
};

// (a + jb)(c + jd) = (ac - bd) + j(ad + bc), each product rounded, then the sum.
[[nodiscard]] constexpr ComplexFloat operator*(ComplexFloat first, ComplexFloat second) noexcept
{
    return ComplexFloat{(first.real * second.real) - (first.imag * second.imag), (first.real * second.imag) + (first.imag * second.real)};
}

[[nodiscard]] constexpr ComplexDouble operator*(ComplexDouble first, ComplexDouble second) noexcept
{
    return ComplexDouble{(first.real * second.real) - (first.imag * second.imag), (first.real * second.imag) + (first.imag * second.real)};
}

[[nodiscard]] constexpr ComplexFloat operator+(ComplexFloat first, ComplexFloat second) noexcept
{
    return ComplexFloat{first.real + second.real, first.imag + second.imag};
}

[[nodiscard]] constexpr ComplexFloat operator-(ComplexFloat first, ComplexFloat second) noexcept
{
    return ComplexFloat{first.real - second.real, first.imag - second.imag};
}

// A real scale of a complex value: (s a) + j(s b).
[[nodiscard]] constexpr ComplexFloat operator*(float scale, ComplexFloat value) noexcept
{
    return ComplexFloat{scale * value.real, scale * value.imag};
}

} // namespace signal_lab
