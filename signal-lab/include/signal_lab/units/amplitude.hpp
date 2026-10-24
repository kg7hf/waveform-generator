// SPDX-License-Identifier: GPL-3.0-only
// SPDX-FileCopyrightText: 2026 Paul R. Decker

#pragma once

// Amplitude: a linear RMS level in full-scale units (1.0 = PCM full scale).
// Guide: docs/modules/signal-lab-watterson.md, "Types and units".
//
// The engine's reference level (StageContext::reference_rms, the RMS of the
// gain-scaled desired signal, E.5.5) and a noise stage's sigma are amplitudes, not
// powers and not decibels. A distinct type keeps a power (R^2) or a dBFS number from
// being passed where an amplitude belongs: awgn_noise_sigma(R^2, snr) does not
// compile. Its decibel form is a Level (dBFS); no stage converts between them, so
// no conversion is defined.

#include <compare>

namespace signal_lab::units
{

class Amplitude
{
public:
    constexpr Amplitude() noexcept = default;

    constexpr explicit Amplitude(double full_scale) noexcept : mFullScale{full_scale} {}

    // The RMS in full-scale units.
    [[nodiscard]] constexpr double full_scale() const noexcept { return mFullScale; }

    [[nodiscard]] constexpr auto operator<=>(const Amplitude&) const noexcept = default;

private:
    double mFullScale{};
};

} // namespace signal_lab::units
