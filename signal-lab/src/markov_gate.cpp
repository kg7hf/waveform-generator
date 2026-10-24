// SPDX-License-Identifier: GPL-3.0-only
// SPDX-FileCopyrightText: 2026 Paul R. Decker

#include "signal_lab/markov_gate.hpp"

#include "signal_lab/det_math.hpp"
#include "signal_lab/stream_rate.hpp"

#include <cmath>
#include <limits>

namespace signal_lab
{
namespace
{

constexpr const char* invalid_message = "cw.markov_on_off means must be finite, at least one frame, and no more than 1e6 seconds";

[[nodiscard]] bool valid_mean(double seconds) noexcept
{
    const double frames = seconds * static_cast<double>(stream_rate.hertz());
    return std::isfinite(seconds) && seconds > 0.0 && std::isfinite(frames) && frames >= 1.0 && frames <= static_cast<double>(markov_gate_max_mean_frames);
}

} // namespace

m110::Status validate_markov_gate_params(const MarkovGateParams& params) noexcept
{
    if (!params.enabled || (valid_mean(params.mean_on_seconds) && valid_mean(params.mean_off_seconds)))
    {
        return m110::Status::success();
    }

    return m110::Status{m110::StatusCode::invalid_configuration, invalid_message};
}

m110::Status MarkovGate::configure(const MarkovGateParams& params, std::uint64_t seed, std::uint32_t index) noexcept
{
    mConfigured = false;

    if (const m110::Status valid = validate_markov_gate_params(params); !valid.is_ok())
    {
        return valid;
    }

    if (!params.enabled)
    {
        mOn = false;
        mDwellFrames = 0U;
        mDwellElapsed = 0U;
        return m110::Status::success();
    }

    mMeanOnFrames = params.mean_on_seconds * static_cast<double>(stream_rate.hertz());
    mMeanOffFrames = params.mean_off_seconds * static_cast<double>(stream_rate.hertz());
    mGenerator = det::Pcg32{seed, markov_gate_rng_family, index};
    mOn = true;
    mDwellElapsed = 0U;
    mDwellFrames = draw_dwell(mMeanOnFrames);
    mConfigured = true;
    return m110::Status::success();
}

std::uint64_t MarkovGate::draw_dwell(double mean_frames) noexcept
{
    const double uniform = mGenerator.uniform();

    if (mean_frames == 1.0)
    {
        return 1U;
    }

    const double probability = 1.0 / mean_frames;
    const double dwell = std::floor(det::log(1.0 - uniform) / det::log(1.0 - probability)) + 1.0;
    return dwell >= static_cast<double>(std::numeric_limits<std::uint64_t>::max()) ? std::numeric_limits<std::uint64_t>::max() : static_cast<std::uint64_t>(dwell);
}

bool MarkovGate::advance() noexcept
{
    if (!mConfigured)
    {
        return false;
    }

    ++mDwellElapsed;

    if (mDwellElapsed < mDwellFrames)
    {
        return false;
    }

    mOn = !mOn;
    mDwellElapsed = 0U;
    mDwellFrames = draw_dwell(mOn ? mMeanOnFrames : mMeanOffFrames);
    return mOn;
}

} // namespace signal_lab
