// SPDX-License-Identifier: GPL-3.0-only
// SPDX-FileCopyrightText: 2026 Paul R. Decker

// The AWGN stage (signal_lab/awgn.hpp): the flat noise band, the N0-referred
// scale and the Box-Muller excitation of design 7.3.
// Guide: docs/modules/signal-lab-engine.md, "AWGN".
//
// Float order (part of the stage's bit-exact definition): each noise sample is
// accumulated from 0.0f over the symmetric pairs from the end taps inwards,
// h[i] * (e(n - 582 + i) + e(n - i)), then the centre tap, then multiplied by the
// float sigma_w and added to the block.

#include "signal_lab/awgn.hpp"

#include "signal_lab/convert.hpp"
#include "signal_lab/det_gaussian.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace signal_lab
{
namespace
{

constexpr m110::Status snr_refusal{.code = m110::StatusCode::invalid_configuration, .message = "awgn.snr_db out of range"};
constexpr m110::Status scratch_refusal{.code = m110::StatusCode::invalid_argument, .message = "awgn needs the engine's scratch storage"};
// sinc(x) = sin(pi x) / (pi x) is 1 at its removable singularity.
constexpr double sinc_origin = 0.0;
// The band-pass is the difference of two low-pass sincs of height 2 f / fs.
constexpr double sinc_height_scale = 2.0;
// fs / 2: a real signal's one-sided band, for the ENBW and for N0 = 2 sigma^2 / fs.
constexpr double one_sided_scale = 2.0;
constexpr m110::Status bandwidth_refusal{.code = m110::StatusCode::invalid_configuration, .message = "awgn.noise_bandwidth is invalid"};
constexpr std::uint8_t noise_band_kernel_count = 4U;

[[nodiscard]] double sinc(double x) noexcept
{
    if (x == sinc_origin)
    {
        return 1.0;
    }

    const double angle = std::numbers::pi * x;
    return det::sin(angle) / angle;
}

// The windowed ideal band-pass at tap index (centre), before the gain normalisation.
[[nodiscard]] double band_tap(std::uint32_t index, double beta, double low, double high, double centre) noexcept
{
    const double rate = as_double(stream_rate.hertz());
    const double offset = as_double(index) - centre;
    const double low_height = sinc_height_scale * low / rate;
    const double high_height = sinc_height_scale * high / rate;
    const double ideal = high_height * sinc(high_height * offset) - low_height * sinc(low_height * offset);
    return ideal * kaiser::window(beta, offset / centre);
}

void design_noise_band(NoiseBandKernel& kernel, NoiseBandwidth bandwidth) noexcept
{
    const double beta = kaiser::beta(noise_band_attenuation);
    const double rate = as_double(stream_rate.hertz());
    double high_cutoff = 0.0;
    double centre = 0.0;
    double reference_hz = 0.0;

    if (bandwidth == NoiseBandwidth::khz3)
    {
        // Keep the original 3 kHz path's exact typed arithmetic and reference.
        const units::FrequencyDelta half_transition = noise_band_transition / 2.0;
        const double low_cutoff = (noise_band_low - half_transition).in(units::Hz);
        high_cutoff = (noise_band_high + half_transition).in(units::Hz);
        centre = as_double(noise_band_centre);
        reference_hz = noise_band_reference.in(units::Hz);
        const double reference = det::two_pi * reference_hz / rate;
        double gain = band_tap(noise_band_centre, beta, low_cutoff, high_cutoff, centre);

        for (std::uint32_t index = 0U; index < noise_band_centre; ++index)
        {
            const double offset = as_double(index) - centre;
            gain += one_sided_scale * band_tap(index, beta, low_cutoff, high_cutoff, centre) * det::cos(reference * offset);
        }

        const std::span<float> half{kernel.half};
        double energy = 0.0;

        for (std::uint32_t index = 0U; index <= noise_band_centre; ++index)
        {
            half[index] = to_float(band_tap(index, beta, low_cutoff, high_cutoff, centre) / gain);
            const double stored = as_double(half[index]);
            energy += (index == noise_band_centre ? 1.0 : one_sided_scale) * stored * stored;
        }

        kernel.enbw = units::FrequencyDelta{rate / one_sided_scale * energy, units::Hz};
        return;
    }

    const units::FrequencyDelta width = noise_band_width(bandwidth);
    const units::Frequency upper_edge = noise_band_low + width;
    const units::Frequency center_frequency = noise_band_low + width / 2.0;
    const units::FrequencyDelta half_transition = noise_band_transition / 2.0;
    const double low_cutoff = (noise_band_low - half_transition).in(units::Hz);
    high_cutoff = (upper_edge + half_transition).in(units::Hz);
    centre = as_double(noise_band_centre);
    reference_hz = center_frequency.in(units::Hz);
    const double reference = det::two_pi * reference_hz / rate;
    // Pass 1: the response at the reference frequency; the kernel is symmetric, so it is real.
    double gain = band_tap(noise_band_centre, beta, low_cutoff, high_cutoff, centre);

    for (std::uint32_t index = 0U; index < noise_band_centre; ++index)
    {
        const double offset = as_double(index) - centre;
        gain += one_sided_scale * band_tap(index, beta, low_cutoff, high_cutoff, centre) * det::cos(reference * offset);
    }

    // Pass 2: store the unit-gain kernel, and its ENBW from the stored values.
    const std::span<float> half{kernel.half};
    double energy = 0.0;

    for (std::uint32_t index = 0U; index <= noise_band_centre; ++index)
    {
        half[index] = to_float(band_tap(index, beta, low_cutoff, high_cutoff, centre) / gain);
        const double stored = as_double(half[index]);
        energy += (index == noise_band_centre ? 1.0 : one_sided_scale) * stored * stored;
    }

    kernel.enbw = units::FrequencyDelta{rate / one_sided_scale * energy, units::Hz};
}

[[nodiscard]] std::array<NoiseBandKernel, noise_band_kernel_count>& noise_band_kernels() noexcept
{
#if defined(WFG_PLAYER_IMAGE)
    [[gnu::section(".ocram.noise_band_kernel")]]
#endif
    static constinit std::array<NoiseBandKernel, noise_band_kernel_count> kernels{};
    return kernels;
}

void ensure_noise_band_kernel(std::uint8_t index, NoiseBandwidth bandwidth) noexcept
{
    // Reuse the former one-byte design flag as a four-kernel ready mask, so the
    // bank costs no additional DTCM state. Configure is single-threaded by contract.
    static_assert(sizeof(bool) == sizeof(std::uint8_t));
    static constinit std::uint8_t designed_mask{};
    const std::uint8_t bit = static_cast<std::uint8_t>(1U << index);

    if ((designed_mask & bit) == 0U)
    {
        design_noise_band(noise_band_kernels()[index], bandwidth);
        designed_mask = static_cast<std::uint8_t>(designed_mask | bit);
    }
}

} // namespace

const NoiseBandKernel& noise_band_kernel() noexcept
{
    // Keep this legacy accessor's reference and its 3 kHz kernel bit-identical.
    ensure_noise_band_kernel(0U, NoiseBandwidth::khz3);
    return noise_band_kernels()[0U];
}

m110::Result<std::reference_wrapper<const NoiseBandKernel>> noise_band_kernel(NoiseBandwidth bandwidth) noexcept
{
    if (!valid_noise_bandwidth(bandwidth))
    {
        return bandwidth_refusal;
    }

    const std::uint8_t index = static_cast<std::uint8_t>(std::to_underlying(bandwidth) / 3000U - 1U);
    ensure_noise_band_kernel(index, bandwidth);
    return std::cref(noise_band_kernels()[index]);
}

units::Amplitude awgn_noise_sigma(units::Amplitude reference, units::Ratio snr) noexcept
{
    // N0 x 3000 Hz = R^2 10^(-SNR / 10) with N0 = 2 sigma^2 / fs.
    const double rate = as_double(stream_rate.hertz());
    return units::Amplitude{reference.full_scale() * units::amplitude_ratio(-snr) * std::sqrt(rate / (one_sided_scale * snr_reference_bandwidth.in(units::Hz)))};
}

m110::Result<units::Amplitude> awgn_noise_sigma(units::Amplitude reference, units::Ratio snr, NoiseBandwidth bandwidth) noexcept
{
    if (!valid_noise_bandwidth(bandwidth))
    {
        return bandwidth_refusal;
    }

    if (bandwidth == NoiseBandwidth::khz3)
    {
        return awgn_noise_sigma(reference, snr);
    }

    const double rate = as_double(stream_rate.hertz());
    const double width = noise_band_width(bandwidth).in(units::Hz);
    return units::Amplitude{reference.full_scale() * units::amplitude_ratio(-snr) * std::sqrt(rate / (one_sided_scale * width))};
}

m110::Status AwgnStage::prepare(const StageContext& context) noexcept
{
    const double snr = mParams.snr.in(units::dB);

    if (!(std::isfinite(snr) && std::abs(snr) <= awgn_max_snr.in(units::dB)))
    {
        return snr_refusal;
    }

    if (context.scratch == nullptr)
    {
        return scratch_refusal;
    }

    const m110::Result<std::reference_wrapper<const NoiseBandKernel>> kernel = noise_band_kernel(context.noise_bandwidth);

    if (!kernel.has_value())
    {
        return kernel.error();
    }

    const m110::Result<units::Amplitude> sigma = awgn_noise_sigma(units::Amplitude{context.reference_rms}, mParams.snr, context.noise_bandwidth);

    if (!sigma.has_value())
    {
        return sigma.error();
    }

    // The legacy window helper and the context speak bare frames and a bare RMS: typed once, here.
    std::uint64_t start = 0U;
    std::uint64_t end = 0U;
    resolve_window(mParams.window, context.total_frames, start, end);
    mStart = units::SampleIndex{start};
    mEnd = units::SampleIndex{end};
    mSigma = sigma.value();
    mSampleScale = to_float(mSigma.full_scale());
    mScratch = std::span<float>{context.scratch, engine_capacity_frames};
    mKernel = &kernel.value().get();
    const StageRandomKey key = context.random_key();
    mGenerator.reseed(key.seed, awgn_rng_family, key.index);
    // Stationary from frame 0: the history holds the excitation of frames -582 ... -1,
    // 291 whole pairs, so frame 0 starts a pair.
    const std::span<float, noise_band_history> history{mHistory};

    for (std::uint32_t index = 0U; index < noise_band_history; index += 2U)
    {
        const det::GaussianPair pair = det::draw_gaussian_pair(mGenerator);
        history[index] = to_float(pair.first);
        history[index + 1U] = to_float(pair.second);
    }

    mCarry = 0.0F;
    mCarryFrame = units::SampleIndex{};
    mCarryPending = false;
    mStats = StageStats{};
    mStats.events_scheduled = 1U;
    return m110::Status::success();
}

float AwgnStage::excitation(units::SampleIndex frame) noexcept
{
    const bool odd = (frame.value() & 1U) != 0U;

    if (odd && mCarryPending && mCarryFrame == frame)
    {
        mCarryPending = false;
        return mCarry;
    }

    const det::GaussianPair pair = det::draw_gaussian_pair(mGenerator);

    if (odd)
    {
        // No carry for this frame (the previous block ended early or repeated it): the
        // fresh pair's first value belongs to frame - 1, which this stream does not hold.
        mCarryPending = false;
        return to_float(pair.second);
    }

    mCarry = to_float(pair.second);
    mCarryFrame = frame + units::SampleCount{1U};
    mCarryPending = true;
    return to_float(pair.first);
}

void AwgnStage::add_chunk(std::span<float> output, units::SampleIndex first) noexcept
{
    const std::span<float> buffer = mScratch.first(noise_band_history + output.size());
    const std::span<const float> kernel{mKernel->half};
    std::ranges::copy(mHistory, buffer.begin());

    for (std::size_t frame = 0U; frame < output.size(); ++frame)
    {
        buffer[noise_band_history + frame] = excitation(first + units::SampleCount{frame});
    }

    for (std::size_t frame = 0U; frame < output.size(); ++frame)
    {
        const std::span<const float> window = buffer.subspan(frame, noise_band_taps);
        float noise = 0.0F;

        for (std::size_t tap = 0U; tap < noise_band_centre; ++tap)
        {
            noise += kernel[tap] * (window[tap] + window[noise_band_history - tap]);
        }

        noise += kernel[noise_band_centre] * window[noise_band_centre];
        const units::SampleIndex absolute = first + units::SampleCount{frame};

        if (absolute >= mStart && absolute < mEnd)
        {
            const float value = noise * mSampleScale;
            output[frame] += value;
            mStats.component_peak = std::max(mStats.component_peak, std::abs(value));
            mStats.component_energy += as_double(value) * as_double(value);
            ++mStats.component_frames;
        }
    }

    std::ranges::copy(buffer.last(noise_band_history), mHistory.begin());
}

m110::Result<std::size_t> AwgnStage::process(float* block, std::size_t frames, std::uint64_t first_input_frame, std::size_t /*capacity*/) noexcept
{
    const std::span<float> output{block, frames};
    const units::SampleIndex first{first_input_frame};

    for (std::size_t done = 0U; done < frames; done += awgn_chunk_frames)
    {
        const std::size_t count = std::min(awgn_chunk_frames, frames - done);
        add_chunk(output.subspan(done, count), first + units::SampleCount{done});
    }

    mStats.events_applied = 1U;
    return frames;
}

} // namespace signal_lab
