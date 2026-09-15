// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Paul R. Decker

#include "signal_lab/det_math.hpp"

#include <cmath>

namespace signal_lab::det
{
namespace
{

// Exact powers of two through the exponent field; std::ldexp is exact but its
// availability/behaviour with -fno-builtin differs, so build the scale directly.
double power_of_two(int exponent) noexcept
{
    double result = 1.0;
    double base = exponent < 0 ? 0.5 : 2.0;
    int count = exponent < 0 ? -exponent : exponent;

    while (count > 0)
    {
        if ((count & 1) != 0)
        {
            result *= base;
        }

        base *= base;
        count >>= 1;
    }

    return result;
}

// Taylor series on |r| <= pi/4 evaluated by Horner's rule (fixed order of operations).
double sin_poly(double r) noexcept
{
    const double r2 = r * r;
    double term = -1.0 / 355687428096000.0; // -1/17!
    term = term * r2 + 1.0 / 1307674368000.0;   // 1/15!
    term = term * r2 - 1.0 / 6227020800.0;      // -1/13!
    term = term * r2 + 1.0 / 39916800.0;        // 1/11!
    term = term * r2 - 1.0 / 362880.0;          // -1/9!
    term = term * r2 + 1.0 / 5040.0;            // 1/7!
    term = term * r2 - 1.0 / 120.0;             // -1/5!
    term = term * r2 + 1.0 / 6.0;               // 1/3!
    return r - r * r2 * term;
}

double cos_poly(double r) noexcept
{
    // cos r = 1 - r^2/2 + r^4 (1/4! - r^2/6! + r^4/8! - ... )
    const double r2 = r * r;
    double term = -1.0 / 6402373705728000.0;    // -1/18!
    term = term * r2 + 1.0 / 20922789888000.0;  // 1/16!
    term = term * r2 - 1.0 / 87178291200.0;     // -1/14!
    term = term * r2 + 1.0 / 479001600.0;       // 1/12!
    term = term * r2 - 1.0 / 3628800.0;         // -1/10!
    term = term * r2 + 1.0 / 40320.0;           // 1/8!
    term = term * r2 - 1.0 / 720.0;             // -1/6!
    term = term * r2 + 1.0 / 24.0;              // 1/4!
    return 1.0 - r2 * (0.5 - r2 * term);
}

std::uint64_t splitmix64(std::uint64_t& state) noexcept
{
    state += 0x9E3779B97F4A7C15ULL;
    std::uint64_t z = state;
    z = (z ^ (z >> 30U)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27U)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31U);
}

} // namespace

double round_half_even(double value) noexcept
{
    const double floored = std::floor(value);
    const double fraction = value - floored;

    if (fraction < 0.5)
    {
        return floored;
    }

    if (fraction > 0.5)
    {
        return floored + 1.0;
    }

    // Exactly half way: choose the even neighbour.
    const double half = floored * 0.5;
    return (half == std::floor(half)) ? floored : floored + 1.0;
}

double exp(double x) noexcept
{
    if (x < -745.0)
    {
        return 0.0;
    }

    if (x > 709.0)
    {
        x = 709.0;
    }

    const double n = std::floor(x / ln2 + 0.5);
    const double r = x - n * ln2;
    // e^r, |r| <= 0.35: Taylor to r^15 / 15!
    double term = 1.0 / 1307674368000.0;
    term = term * r + 1.0 / 87178291200.0;
    term = term * r + 1.0 / 6227020800.0;
    term = term * r + 1.0 / 479001600.0;
    term = term * r + 1.0 / 39916800.0;
    term = term * r + 1.0 / 3628800.0;
    term = term * r + 1.0 / 362880.0;
    term = term * r + 1.0 / 40320.0;
    term = term * r + 1.0 / 5040.0;
    term = term * r + 1.0 / 720.0;
    term = term * r + 1.0 / 120.0;
    term = term * r + 1.0 / 24.0;
    term = term * r + 1.0 / 6.0;
    term = term * r + 0.5;
    term = term * r + 1.0;
    term = term * r + 1.0;
    return term * power_of_two(static_cast<int>(n));
}

double log(double x) noexcept
{
    if (!(x > 0.0))
    {
        return -1.0e300;
    }

    int exponent = 0;
    double mantissa = std::frexp(x, &exponent); // exact: mantissa in [0.5, 1)

    if (mantissa < 0.7071067811865476)
    {
        mantissa *= 2.0; // exact
        exponent -= 1;
    }

    // log(m) = 2 * atanh(z), z = (m - 1) / (m + 1), |z| <= 0.1716
    const double z = (mantissa - 1.0) / (mantissa + 1.0);
    const double z2 = z * z;
    double term = 1.0 / 27.0;
    term = term * z2 + 1.0 / 25.0;
    term = term * z2 + 1.0 / 23.0;
    term = term * z2 + 1.0 / 21.0;
    term = term * z2 + 1.0 / 19.0;
    term = term * z2 + 1.0 / 17.0;
    term = term * z2 + 1.0 / 15.0;
    term = term * z2 + 1.0 / 13.0;
    term = term * z2 + 1.0 / 11.0;
    term = term * z2 + 1.0 / 9.0;
    term = term * z2 + 1.0 / 7.0;
    term = term * z2 + 1.0 / 5.0;
    term = term * z2 + 1.0 / 3.0;
    term = term * z2 + 1.0;
    return static_cast<double>(exponent) * ln2 + 2.0 * z * term;
}

double sin(double x) noexcept
{
    const double k = std::floor(x / half_pi + 0.5);
    const double r = x - k * half_pi;
    const auto quadrant = static_cast<std::uint64_t>(static_cast<std::int64_t>(k)) & 3U;

    switch (quadrant)
    {
        case 0U:
            return sin_poly(r);

        case 1U:
            return cos_poly(r);

        case 2U:
            return -sin_poly(r);

        default:
            return -cos_poly(r);
    }
}

double cos(double x) noexcept
{
    const double k = std::floor(x / half_pi + 0.5);
    const double r = x - k * half_pi;
    const auto quadrant = static_cast<std::uint64_t>(static_cast<std::int64_t>(k)) & 3U;

    switch (quadrant)
    {
        case 0U:
            return cos_poly(r);

        case 1U:
            return -sin_poly(r);

        case 2U:
            return -cos_poly(r);

        default:
            return sin_poly(r);
    }
}

double db_to_amplitude(double db) noexcept
{
    return exp(db * (ln10 / 20.0));
}

double amplitude_to_db(double amplitude) noexcept
{
    return amplitude > 0.0 ? 20.0 * log(amplitude) / ln10 : -1.0e300;
}

std::uint64_t seconds_to_frames(double seconds, std::uint32_t sample_rate_hz) noexcept
{
    if (!(seconds > 0.0))
    {
        return 0U;
    }

    const double frames = round_half_even(seconds * static_cast<double>(sample_rate_hz));

    if (!std::isfinite(frames) || frames >= 18446744073709551616.0)
    {
        return UINT64_MAX;
    }

    return static_cast<std::uint64_t>(frames);
}

waveform_generator::audio::Pcm24Sample quantize_pcm24(float sample) noexcept
{
    if (std::isnan(sample))
    {
        return 0;
    }

    double rounded = round_half_even(static_cast<double>(sample) *
                                     waveform_generator::audio::pcm24_scale);

    if (rounded < waveform_generator::audio::pcm24_min)
    {
        rounded = waveform_generator::audio::pcm24_min;
    }
    else if (rounded > waveform_generator::audio::pcm24_max)
    {
        rounded = waveform_generator::audio::pcm24_max;
    }

    return static_cast<waveform_generator::audio::Pcm24Sample>(rounded);
}

Pcg32::Pcg32(std::uint64_t seed, std::uint32_t family, std::uint32_t index) noexcept
{
    reseed(seed, family, index);
}

void Pcg32::reseed(std::uint64_t seed, std::uint32_t family, std::uint32_t index) noexcept
{
    std::uint64_t mix = seed ^ (static_cast<std::uint64_t>(family) << 32U) ^ (static_cast<std::uint64_t>(index) * 0x9E3779B97F4A7C15ULL);
    const std::uint64_t initial_state = splitmix64(mix);
    const std::uint64_t stream = splitmix64(mix);
    increment_ = (stream << 1U) | 1U;
    state_ = 0U;
    (void)next();
    state_ += initial_state;
    (void)next();
}

std::uint32_t Pcg32::next() noexcept
{
    const std::uint64_t old = state_;
    state_ = old * 6364136223846793005ULL + increment_;
    const auto xorshifted = static_cast<std::uint32_t>(((old >> 18U) ^ old) >> 27U);
    const auto rotation = static_cast<std::uint32_t>(old >> 59U);
    return (xorshifted >> rotation) | (xorshifted << ((32U - rotation) & 31U));
}

double Pcg32::uniform() noexcept
{
    return static_cast<double>(next() >> 8U) * (1.0 / 16777216.0);
}

double Pcg32::gaussian() noexcept
{
    double sum = 0.0;

    for (int draw = 0; draw < 12; ++draw)
    {
        sum += uniform();
    }

    return sum - 6.0;
}

double Pcg32::exponential(double mean) noexcept
{
    const double u = uniform();
    return -mean * log(1.0 - u);
}

void StreamDigest::reset() noexcept
{
    hash_ = 0xCBF29CE484222325ULL;
}

void StreamDigest::update(const std::int16_t* samples, std::size_t count) noexcept
{
    std::uint64_t hash = hash_;

    for (std::size_t index = 0U; index < count; ++index)
    {
        const auto bits = static_cast<std::uint16_t>(samples[index]);
        hash ^= static_cast<std::uint64_t>(bits & 0xFFU);
        hash *= 0x100000001B3ULL;
        hash ^= static_cast<std::uint64_t>(bits >> 8U);
        hash *= 0x100000001B3ULL;
    }

    hash_ = hash;
}

void StreamDigest::update(const float* samples, std::size_t count) noexcept
{
    std::uint64_t hash = hash_;

    for (std::size_t index = 0U; index < count; ++index)
    {
        const auto bits = static_cast<std::uint32_t>(quantize_pcm24(samples[index])) &
                          0x00FFFFFFU;

        for (std::uint32_t byte = 0U; byte < 3U; ++byte)
        {
            hash ^= static_cast<std::uint64_t>((bits >> (8U * byte)) & 0xFFU);
            hash *= 0x100000001B3ULL;
        }
    }

    hash_ = hash;
}

} // namespace signal_lab::det
