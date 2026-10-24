// SPDX-License-Identifier: GPL-3.0-only
// SPDX-FileCopyrightText: 2026 Paul R. Decker

#pragma once

// The engine's stream rate as a typed rate, and its only definition. Every size of
// the new stages is a constexpr function of it (design 3.7), and scenario.hpp's
// sample_rate_hz (the legacy stages, the scenario ranges, the hosts' WAV headers)
// is defined from it: another rate is one compile-time change, never a runtime one.

#include "signal_lab/units/sampling.hpp"

namespace signal_lab
{

// 48 kHz: the RT1170 codec and USB stream rate, and the lane's capture rate.
inline constexpr units::SampleRate stream_rate{48000U};

} // namespace signal_lab
