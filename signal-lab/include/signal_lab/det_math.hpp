#pragma once

// Deterministic arithmetic helpers for the portable impairment engine.
//
// The host renderer and the RT1170 player must produce the same sample
// stream for the same scenario, so nothing here depends on the C library's
// transcendental functions (their last-bit results differ between newlib,
// MinGW and glibc). Every routine below is built from IEEE-754 double
// add/multiply/divide/sqrt, which round identically on every conforming
// target when floating-point contraction is disabled (-ffp-contract=off).

#include "common/pcm24.hpp"

#include <cstddef>
#include <cstdint>

namespace signal_lab::det
{

inline constexpr double pi = 3.141592653589793238462643383279502884;
inline constexpr double two_pi = 6.283185307179586476925286766559005768;
inline constexpr double half_pi = 1.570796326794896619231321691639751442;
inline constexpr double ln2 = 0.693147180559945309417232121458176568;
inline constexpr double ln10 = 2.302585092994045684017991454684364208;

// Round half to even (the NumPy / Python 3 convention used by the Phase 1 engine).
[[nodiscard]] double round_half_even(double value) noexcept;

// e^x for |x| <= 700, |relative error| < 1e-14.
[[nodiscard]] double exp(double x) noexcept;

// Natural logarithm for x > 0, |relative error| < 1e-14.
[[nodiscard]] double log(double x) noexcept;

// sin / cos for any finite argument (argument reduction by pi/2 quadrants).
[[nodiscard]] double sin(double x) noexcept;
[[nodiscard]] double cos(double x) noexcept;

// 10^(db/20) and 20*log10(amplitude).
[[nodiscard]] double db_to_amplitude(double db) noexcept;
[[nodiscard]] double amplitude_to_db(double amplitude) noexcept;

// Seconds -> whole frames, half-to-even (matches signal_lab.stream.seconds_to_frames).
[[nodiscard]] std::uint64_t seconds_to_frames(double seconds, std::uint32_t sample_rate_hz) noexcept;

// Quantize a normalized sample to signed packed-PCM24 code space. The returned
// value is right-aligned in int32_t; container packing is a boundary concern.
[[nodiscard]] waveform_generator::audio::Pcm24Sample quantize_pcm24(float sample) noexcept;

// PCG32 (XSH-RR) with the (seed, family, index) keying used by the scenarios.
class Pcg32
{
public:
    Pcg32() noexcept = default;
    Pcg32(std::uint64_t seed, std::uint32_t family, std::uint32_t index) noexcept;

    void reseed(std::uint64_t seed, std::uint32_t family, std::uint32_t index) noexcept;

    [[nodiscard]] std::uint32_t next() noexcept;

    // Uniform in [0, 1) with 24 significant bits (exactly representable as float and double).
    [[nodiscard]] double uniform() noexcept;

    // Standard normal approximation: sum of twelve uniforms minus six (Irwin-Hall).
    // Bounded to [-6, 6]; the band-limiting FIR that follows it in every model
    // sums hundreds of these, so the output is Gaussian to well beyond 4 sigma.
    [[nodiscard]] double gaussian() noexcept;

    // Exponential variate with the given mean (inter-arrival times).
    [[nodiscard]] double exponential(double mean) noexcept;

private:
    std::uint64_t state_{0x853C49E6748FEA9BULL};
    std::uint64_t increment_{0xDA3E39CB94B95BDBULL};
};

// FNV-1a 64-bit running digest. The legacy overload hashes little-endian
// PCM16. The normalized-float overload quantizes to canonical packed PCM24.
class StreamDigest
{
public:
    void reset() noexcept;
    void update(const std::int16_t* samples, std::size_t count) noexcept;
    void update(const float* samples, std::size_t count) noexcept;
    [[nodiscard]] std::uint64_t value() const noexcept
    {
        return hash_;
    }

private:
    std::uint64_t hash_{0xCBF29CE484222325ULL};
};

} // namespace signal_lab::det
