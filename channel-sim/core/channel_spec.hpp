// SPDX-License-Identifier: GPL-3.0-only
// SPDX-FileCopyrightText: 2026 Paul R. Decker

// The channel a pathsim job simulates (guide: docs/modules/pathsim.md).
//
// ChannelSpec is heap-free and owns no pathsim type. It is built with the
// semantics of the patched pathsim command line (rev-6 main.cpp), so the same
// option set gives the same channel:
//  1. a preset (--ccir-poor, --direct, ...) replaces everything; it comes first;
//  2. --snr enables AWGN at that label;
//  3. --spread/--offset (path 1), --delay2/--spread2/--offset2 (path 2) and
//     --delay3/--spread3/--offset3 (path 3) set one field of the addressed path;
//     addressing a path that does not exist yet adds default paths (delay 0,
//     spread 0, offset 0) up to it.
// pathsim itself checks no range. validate() rejects what pathsim would clamp or
// index out of bounds, so a job never starts with a value pathsim cannot handle,
// and otherwise accepts what pathsim.exe accepts.

#pragma once

#include "common/status.hpp"
#include "core/convert.hpp"
#include "core/units.hpp"

#include <array>
#include <cstddef>
#include <span>
#include <string_view>

namespace channel_sim
{

// The command line addresses three path slots (pathsim's processor takes any count).
inline constexpr std::size_t max_paths = 3U;

// pathsim's delay line is a ring of Delay::BUFSIZE = 2048 + 400 complex samples
// (channel_spec.cpp checks the number against pathsim's). Delay::add_delay reads
// int(8 * ms) samples behind the write position, and writes and reads alternate
// sample by sample, so every lag 0 .. 2447 stays inside the ring and is exact.
// Upstream's "50 ms" (MAXDELAY) is only how the ring was sized; it is not a bound
// pathsim enforces or needs. The limit is therefore 8 * ms < 2448: a path delay
// must be below 306 ms (305.875 ms is the last 0.125 ms step). A negative delay
// is refused (-0.125 ms and below would index past the ring).
inline constexpr std::size_t pathsim_delay_ring_samples = 2448U;
inline constexpr double pathsim_samples_per_ms = 8.0;
inline constexpr Milliseconds path_delay_limit{306.0}; // exclusive
static_assert(path_delay_limit.value * pathsim_samples_per_ms == as_double(pathsim_delay_ring_samples), "the delay limit is the ring's length in samples");
// pathsim's Rayleigh generator clamps the spread to 0..30 Hz.
inline constexpr Hertz max_spread{30.0};
// A frequency offset beyond Nyquist at 8 kHz is meaningless.
inline constexpr Hertz max_offset{4000.0};
// A label beyond this is not a channel; it also keeps pow(10, snr / 20) finite.
inline constexpr Decibels max_snr_magnitude{150.0};
// pathsim's NoiseParams default; recorded in the truth export even without AWGN.
inline constexpr Decibels pathsim_default_snr{10.0};

struct PathSpec
{
    Milliseconds delay{}; // 0 <= delay < path_delay_limit; path 1 has none (no option sets it)
    Hertz spread{};       // two-sigma Doppler spread; below 0.1 Hz the path does not fade
    Hertz offset{};       // frequency offset
};

struct NoiseSpec
{
    bool enabled{false};
    Decibels snr{pathsim_default_snr};
};

class ChannelSpec
{
public:
    // Copies pathsim's preset of that command-line name (default_params()). Only one
    // preset per channel, and before any other setter.
    [[nodiscard]] m110::Status apply_preset(std::string_view name) noexcept;

    // --snr: AWGN on, at this label (delivered = label + 0.42 dB, lane calibration).
    void set_snr(Decibels snr) noexcept;

    // slot 0 is path 1 (--spread/--offset); slots 1 and 2 are paths 2 and 3.
    // There is no delay for slot 0, as in pathsim's command line.
    [[nodiscard]] m110::Status set_delay(std::size_t slot, Milliseconds delay) noexcept;
    [[nodiscard]] m110::Status set_spread(std::size_t slot, Hertz spread) noexcept;
    [[nodiscard]] m110::Status set_offset(std::size_t slot, Hertz offset) noexcept;

    // Every range pathsim relies on; see the limits above.
    [[nodiscard]] m110::Status validate() const noexcept;

    [[nodiscard]] std::span<const PathSpec> paths() const noexcept;
    [[nodiscard]] const NoiseSpec& noise() const noexcept;
    // The preset's command-line name, or empty. Points into pathsim's static table.
    [[nodiscard]] std::string_view preset() const noexcept;

private:
    // Adds default paths up to slot (pathsim main.cpp's push_back loop).
    [[nodiscard]] m110::Status reach(std::size_t slot) noexcept;
    [[nodiscard]] PathSpec& slot_ref(std::size_t slot) noexcept;

    std::array<PathSpec, max_paths> paths_{};
    std::size_t path_count_{};
    NoiseSpec noise_{};
    std::string_view preset_{};
    bool modified_{};
};

// pathsim's presets, in its table order (for --help).
[[nodiscard]] std::size_t preset_count() noexcept;
[[nodiscard]] std::string_view preset_name(std::size_t index) noexcept;
[[nodiscard]] std::string_view preset_title(std::size_t index) noexcept;
[[nodiscard]] bool is_preset(std::string_view name) noexcept;

} // namespace channel_sim
