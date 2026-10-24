// SPDX-License-Identifier: GPL-3.0-only
// SPDX-FileCopyrightText: 2026 Paul R. Decker

#include "signal_lab/cw_fsk.hpp"

#include "signal_lab/det_math.hpp"
#include "signal_lab/stream_rate.hpp"

#include <cmath>

namespace signal_lab
{
namespace
{

constexpr const char* invalid_baud_message = "cw.fsk.baud must produce a positive whole-sample symbol period within 1e-6 frame and 1e9 seconds";
constexpr const char* invalid_frequencies_message = "cw.fsk requires finite center/shift and both tones in [1, 23999] Hz";
constexpr const char* invalid_pattern_message = "cw.fsk.pattern is invalid";
constexpr double phase_turn = 18446744073709551616.0;

} // namespace

m110::Result<std::uint64_t> cw_fsk_symbol_frames(const FskParams& params) noexcept
{
    const double baud = params.baud;

    if (!std::isfinite(baud) || !(baud > 0.0) || baud > static_cast<double>(stream_rate.hertz()))
    {
        return m110::Status{m110::StatusCode::invalid_configuration, invalid_baud_message};
    }

    const double exact_frames = static_cast<double>(stream_rate.hertz()) / baud;

    if (!std::isfinite(exact_frames))
    {
        return m110::Status{m110::StatusCode::invalid_configuration, invalid_baud_message};
    }

    const double rounded = det::round_half_even(exact_frames);

    if (exact_frames < 1.0 || exact_frames > static_cast<double>(cw_fsk_max_symbol_frames) || std::abs(exact_frames - rounded) > 1.0e-6)
    {
        return m110::Status{m110::StatusCode::invalid_configuration, invalid_baud_message};
    }

    return static_cast<std::uint64_t>(rounded);
}

m110::Status validate_cw_fsk_params(const FskParams& params, double center_hz) noexcept
{
    if (!params.enabled)
    {
        return m110::Status::success();
    }

    const m110::Result<std::uint64_t> symbol_frames = cw_fsk_symbol_frames(params);

    if (!symbol_frames.has_value())
    {
        return symbol_frames.error();
    }

    if (params.pattern != FskPattern::alternating && params.pattern != FskPattern::random)
    {
        return m110::Status{m110::StatusCode::invalid_configuration, invalid_pattern_message};
    }

    const double shift = params.shift.in(units::Hz);
    const double mark_hz = center_hz + shift / 2.0;
    const double space_hz = center_hz - shift / 2.0;

    if (!std::isfinite(center_hz) || !std::isfinite(shift) || !(shift > 0.0) || !std::isfinite(mark_hz) || !std::isfinite(space_hz) || mark_hz < 1.0 || mark_hz > 23999.0 || space_hz < 1.0 ||
        space_hz > 23999.0 || !(space_hz < mark_hz))
    {
        return m110::Status{m110::StatusCode::invalid_configuration, invalid_frequencies_message};
    }

    const double mark_step = det::round_half_even(mark_hz * phase_turn / static_cast<double>(stream_rate.hertz()));
    const double space_step = det::round_half_even(space_hz * phase_turn / static_cast<double>(stream_rate.hertz()));

    if (mark_step == space_step)
    {
        return m110::Status{m110::StatusCode::invalid_configuration, "cw.fsk tones must have distinct Q0.64 phase steps"};
    }

    return m110::Status::success();
}

} // namespace signal_lab
