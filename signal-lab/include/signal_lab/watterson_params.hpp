// SPDX-License-Identifier: GPL-3.0-only
// SPDX-FileCopyrightText: 2026 Paul R. Decker

#pragma once

// The parameters of the Watterson multipath fading stage and their ranges.
// Guide: docs/modules/signal-lab-watterson.md, "Scenario schema".
//
// Small on purpose: the scenario holds one WattersonParams. Every value carries
// its unit (units/*.hpp); the ranges cite their sources.

#include "signal_lab/convert.hpp"
#include "signal_lab/stream_rate.hpp"
#include "signal_lab/units/angle.hpp"
#include "signal_lab/units/decibel.hpp"
#include "signal_lab/units/duration.hpp"
#include "signal_lab/units/frequency.hpp"
#include "signal_lab/units/sampling.hpp"

#include <array>
#include <cstdint>

namespace signal_lab
{

// At most 4 paths: MIL-STD-188-110D D.6.2 Table D-LXV (at most 4), lane amendment 1
// (at least 4), survey item B2.
inline constexpr std::uint32_t watterson_max_paths = 4U;
// Delays 0 to 20 ms: survey gap A1, ITU-R F.1487 Annex 2 (0-20 ms), at least the
// 9 ms of D.6.2.
inline constexpr units::Duration watterson_max_delay{20.0, units::ms};
// Two-sigma Doppler spreads 0.1 to 10 Hz: lane criteria 2 and 7; E.7.3's 10 Hz test.
inline constexpr units::FrequencyDelta watterson_min_spread{0.1, units::Hz};
inline constexpr units::FrequencyDelta watterson_max_spread{10.0, units::Hz};
// Shifts up to +/-75 Hz: MIL-STD-188-110B C.6.3, 110D D.6.4.
inline constexpr units::FrequencyDelta watterson_max_shift{75.0, units::Hz};
// Relative path levels within +/-60 dB: the design range (design 4.3).
inline constexpr units::Gain watterson_max_relative_level{60.0, units::dB};
// A fixed path's phase within +/-360 degrees, the range of the other stages' phases.
inline constexpr units::Angle watterson_max_phase{360.0, units::degrees};

// The longest delay in frames at the stream rate: 20 ms x 48 kHz = 960 frames.
inline constexpr double watterson_max_delay_ms = watterson_max_delay.in(units::ms);
inline constexpr units::SampleCount watterson_max_delay_frames{whole_to_uint64(watterson_max_delay_ms * as_double(stream_rate.hertz()) / units::milliseconds_per_second)};
static_assert(as_double(watterson_max_delay_frames.value()) * units::milliseconds_per_second == watterson_max_delay_ms * as_double(stream_rate.hertz()),
              "the longest delay is a whole number of frames");

enum class PathFading : std::uint8_t
{
    rayleigh,
    fixed,
};

struct WattersonPath
{
    units::Duration delay{};                 // rounded to the nearest frame (E.5.2)
    units::Gain level{};                     // relative; the set is normalised to a total of 1 (E.5.4)
    PathFading fading{PathFading::rayleigh}; // a Rician path is a fixed and a Rayleigh path at one delay
    units::FrequencyDelta spread{};          // two-sigma Doppler spread (E.5.4: sigma = d / 2), rayleigh only
    units::FrequencyDelta shift{};           // frequency shift of the path, any kind
    units::Angle phase{};                    // phase of a fixed path (D.6.2 states no convention)
};

struct WattersonParams
{
    std::array<WattersonPath, watterson_max_paths> paths{};
    std::uint32_t path_count{};
};

} // namespace signal_lab
