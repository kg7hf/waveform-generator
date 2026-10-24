// SPDX-License-Identifier: GPL-3.0-only
// SPDX-FileCopyrightText: 2026 Paul R. Decker

#include "signal_lab/cw_fading.hpp"

#include "signal_lab/det_math.hpp"
#include "signal_lab/watterson.hpp"

#include <cmath>

namespace signal_lab
{
namespace
{

constexpr const char* invalid_spread_message = "cw.fading.spread_hz must be finite and in [0.1, 10] Hz";

} // namespace

m110::Status validate_cw_fading_params(const CwFadingParams& params) noexcept
{
    if (!params.enabled)
    {
        return m110::Status::success();
    }

    const double spread_hz = params.spread.in(units::Hz);

    if (!std::isfinite(spread_hz) || spread_hz < watterson_min_spread.in(units::Hz) || spread_hz > watterson_max_spread.in(units::Hz))
    {
        return m110::Status{m110::StatusCode::invalid_configuration, invalid_spread_message};
    }

    return m110::Status::success();
}

m110::Status configure_cw_fading(WattersonPathState& state, const CwFadingParams& params, std::uint64_t seed, std::uint32_t index) noexcept
{
    if (const m110::Status valid = validate_cw_fading_params(params); !valid.is_ok())
    {
        return valid;
    }

    WattersonPath path{};
    path.fading = PathFading::rayleigh;
    path.spread = params.spread;
    WattersonPathReport report{};
    const WattersonPathState::Setup setup{.path = &path, .power = 1.0, .index = index, .generator = det::Pcg32{seed, cw_fading_rng_family, index}, .observer = nullptr};
    return state.configure_rayleigh(setup, report);
}

} // namespace signal_lab
