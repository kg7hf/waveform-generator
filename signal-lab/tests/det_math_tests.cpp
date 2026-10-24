// SPDX-License-Identifier: GPL-3.0-only
// SPDX-FileCopyrightText: 2026 Paul R. Decker

// Catch2 unit tests of signal-lab's deterministic arithmetic, signal_lab/det_math.hpp.
// Guide: docs/modules/signal-lab-engine.md, "Catch2 unit tests".
//
// The repository's first Catch2 suite, and the pattern for the per-stage suites:
// one property per case, every constant named with its source, and floating-point
// results compared within a stated tolerance, never against one C library's last
// bit. signal_lab_tests.cpp keeps its wide sweeps against the C library; these
// cases pin the exact anchor values, rounding, ranges and keyed random streams
// that the stages build on, with known-answer vectors from an independent
// implementation for the streams and the digests.

#include "common/pcm24.hpp"
#include "signal_lab/convert.hpp"
#include "signal_lab/det_math.hpp"
#include "signal_lab/impairment.hpp"
#include "signal_lab/scenario.hpp"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>

namespace
{

namespace det = signal_lab::det;
namespace audio = waveform_generator::audio;
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;

// det_math.hpp: log, and exp over the float range, keep |relative error| < 1e-14.
constexpr double exp_log_relative_bound = 1.0e-14;
// The float range of the engine's samples: e^x overflows float above
// ln(FLT_MAX) = 88.72 (IEEE-754 binary32).
constexpr double float_range_exponent = 88.0;
// det_math.hpp: exp over its whole domain, |x| <= 700, keeps |relative error| < 1e-13,
// the bound signal_lab_tests.cpp also uses. The rounding of n ln2 in the range
// reduction grows with |x| and dominates there.
constexpr double exp_domain_limit = 700.0;
constexpr double exp_domain_relative_bound = 1.0e-13;
// The absolute bound signal_lab_tests.cpp holds sin and cos to against the C
// library over its sweep, |x| <= 34.6 rad. The rounding of k pi/2 in the
// quadrant reduction grows the error with |x| beyond that.
constexpr double sin_cos_absolute_bound = 2.0e-14;
constexpr double sin_cos_sweep_limit = 34.6;
// sin^2 + cos^2 - 1 with each factor within sin_cos_absolute_bound of a value in
// [-1, 1]: 2 |sin| e + 2 |cos| e <= 4 e, ignoring the e^2 terms.
constexpr double pythagorean_bound = 4.0 * sin_cos_absolute_bound;

// The definition of the decibel for amplitudes: 20 log10(10) = 20 dB.
constexpr double decade_db = 20.0;
constexpr double decade_amplitude = 10.0;
// One conversion: the exp or log bound plus the roundings of the 20 / ln 10 scale
// (a few 1e-16), so twice the bound holds with room to spare.
constexpr double decibel_relative_bound = 2.0 * exp_log_relative_bound;
// A dB level through amplitude and back: the exp and log bounds give an error of
// (20 / ln 10) e + |level| e dB, 1.3e-12 dB at |level| = 120 dB.
constexpr double db_round_trip_bound = 2.0e-12;

// det_math.hpp: uniform() has 24 significant bits, so u * 2^24 is a whole number.
constexpr double uniform_steps = 16777216.0;
// det_math.hpp: gaussian() is twelve uniforms minus six (Irwin-Hall), bounded to [-6, 6].
constexpr double gaussian_bound = 6.0;
// Draws per stream in the keyed-stream and range cases: enough to cross many
// 32-bit outputs, small enough to keep the case instant.
constexpr std::size_t stream_check_draws = 64U;
constexpr int range_check_draws = 10000;

// A value and its round-half-to-even result.
struct Rounding
{
    double value;
    double expected;
};

constexpr std::array rounding_cases{
    Rounding{0.5, 0.0},    // tie, down to the even neighbour
    Rounding{1.5, 2.0},    // tie, up to the even neighbour
    Rounding{2.5, 2.0},    // tie, down
    Rounding{-0.5, 0.0},   // negative tie, up to zero
    Rounding{-1.5, -2.0},  // negative tie, down
    Rounding{-2.5, -2.0},  // negative tie, up
    Rounding{2.4999, 2.0}, // below the tie
    Rounding{2.5001, 3.0}, // above the tie
};

// A scenario key (seed, family, index) as the stages pass it to Pcg32.
struct StreamKey
{
    std::uint64_t seed;
    std::uint32_t family;
    std::uint32_t index;
};

constexpr StreamKey reference_key{20261008U, 1U, 0U};

// Known-answer vectors (QA-006, issue #10). An independent Python implementation of
// the algorithms this header documents (splitmix64 keying, PCG32 XSH-RR, the 24-bit
// uniform, the twelve-uniform Gaussian, the exponential, FNV-1a over little-endian
// bytes, round half to even and the PCM24 clamp) gave these values, and det_math.cpp
// matched every one bit for bit. They pin values, not only properties: a changed
// multiplier, draw or digest definition changes every seeded stream and digest the
// lane compares across builds.
//
// The first six next() outputs of the stream with this key.
constexpr StreamKey known_answer_key{0x123456789ABCDEF0U, 7U, 3U};
constexpr std::array known_answer_draws{std::uint32_t{2543730154U}, std::uint32_t{1506303243U}, std::uint32_t{2663355352U},
                                        std::uint32_t{2899136656U}, std::uint32_t{16925385U},   std::uint32_t{2553874363U}};
// The first three uniform() values of the stream with this key, and the first
// gaussian() and exponential() of a fresh stream with the same key. The uniforms are
// k 2^-24 and the Gaussian a sum of twelve of them less 6 (28 significant bits at
// most), so all four are exact doubles; the exponential goes through det::log.
constexpr StreamKey unit_key{1U, 0U, 0U};
constexpr std::array known_answer_uniforms{0.32143813371658325, 0.006188631057739258, 0.9046539664268494};
constexpr double known_answer_gaussian = 0.65506911277771;
constexpr double known_answer_exponential_mean = 2.0;
// -2 ln(1 - u) for the first uniform above; -2 ln(u) would be 2.2699.
constexpr double known_answer_exponential = 0.7755592458018264;
// Both StreamDigest overloads from reset(): the float one hashes the three bytes of
// each canonical PCM24 code, the PCM16 one the two bytes of each sample.
constexpr std::array digest_floats{0.0F, 0.5F, -0.5F, 1.0F, -1.0F, 0.123456F};
constexpr std::uint64_t digest_floats_value = 0x95BAC985500FABE2U;
constexpr std::array digest_pcm16{std::int16_t{0}, std::int16_t{1}, std::int16_t{-1}, std::int16_t{32767}, std::int16_t{-32768}, std::int16_t{1234}};
constexpr std::uint64_t digest_pcm16_value = 0x273D2C893718543EU;

// A duration in seconds and the whole frames seconds_to_frames() gives at the
// engine's rate.
struct FrameRounding
{
    double seconds;
    std::uint64_t frames;
};

constexpr double frame_seconds = 1.0 / signal_lab::as_double(signal_lab::sample_rate_hz);
constexpr std::array half_frame_cases{
    FrameRounding{0.5 * frame_seconds, 0U}, // 0.5 frames, a tie: down to the even neighbour
    FrameRounding{1.5 * frame_seconds, 2U}, // 1.5 frames, a tie: up to the even neighbour (floor gives 1)
    FrameRounding{2.5 * frame_seconds, 2U}, // 2.5 frames, a tie: down (round half up gives 3)
    FrameRounding{0.0000104, 0U},           // 0.4992 frames, below the tie
    FrameRounding{0.0000106, 1U},           // 0.5088 frames, above the tie (floor gives 0)
};

// The first stream_check_draws outputs of the stream with that key.
[[nodiscard]] std::array<std::uint32_t, stream_check_draws> first_draws(const StreamKey& key) noexcept
{
    det::Pcg32 generator(key.seed, key.family, key.index);
    std::array<std::uint32_t, stream_check_draws> draws{};

    for (std::uint32_t& draw : draws)
    {
        draw = generator.next();
    }

    return draws;
}

} // namespace

TEST_CASE("exp and log are exact at powers of two", "[det_math]")
{
    // Range reduction by k ln2 leaves a zero remainder, so these hold in every
    // IEEE-754 double build with contraction off, host or board.
    constexpr std::array exponents{-8, -3, -1, 0, 1, 2, 5, 10};

    for (const int exponent : exponents)
    {
        const double power = std::ldexp(1.0, exponent);
        const double logarithm = signal_lab::as_double(exponent) * det::ln2;
        CHECK(det::exp(logarithm) == power);
        CHECK(det::log(power) == logarithm);
    }
}

TEST_CASE("exp keeps its stated relative error over the float range", "[det_math]")
{
    constexpr std::array arguments{-float_range_exponent, -20.25, -0.7, 0.1, 0.35, 5.5, 42.0, float_range_exponent};

    for (const double x : arguments)
    {
        CHECK_THAT(det::exp(x), WithinRel(std::exp(x), exp_log_relative_bound));
    }
}

TEST_CASE("exp keeps its stated relative error over its whole domain", "[det_math]")
{
    constexpr std::array arguments{-exp_domain_limit, -300.0, 300.0, exp_domain_limit};

    for (const double x : arguments)
    {
        CHECK_THAT(det::exp(x), WithinRel(std::exp(x), exp_domain_relative_bound));
    }
}

TEST_CASE("log keeps its stated relative error", "[det_math]")
{
    constexpr std::array arguments{1.0e-300, 1.0e-9, 0.01, 0.75, 1.5, 3.0, 1.0e6, 1.0e300};

    for (const double x : arguments)
    {
        CHECK_THAT(det::log(x), WithinRel(std::log(x), exp_log_relative_bound));
    }
}

TEST_CASE("sin and cos stay within the engine's bound and on the unit circle", "[det_math]")
{
    CHECK(det::sin(0.0) == 0.0);
    CHECK(det::cos(0.0) == 1.0);
    constexpr std::array arguments{-sin_cos_sweep_limit, -det::pi, -1.0, det::half_pi, 2.5, det::two_pi, sin_cos_sweep_limit};

    for (const double x : arguments)
    {
        const double sine = det::sin(x);
        const double cosine = det::cos(x);
        CHECK_THAT(sine, WithinAbs(std::sin(x), sin_cos_absolute_bound));
        CHECK_THAT(cosine, WithinAbs(std::cos(x), sin_cos_absolute_bound));
        CHECK_THAT(sine * sine + cosine * cosine, WithinAbs(1.0, pythagorean_bound));
    }
}

TEST_CASE("round_half_even sends ties to the even neighbour", "[det_math]")
{
    for (const Rounding& rounding : rounding_cases)
    {
        CHECK(det::round_half_even(rounding.value) == rounding.expected);
    }
}

TEST_CASE("decibel conversions follow 20 log10 and invert each other", "[det_math]")
{
    CHECK(det::db_to_amplitude(0.0) == 1.0);
    CHECK(det::amplitude_to_db(1.0) == 0.0);
    CHECK_THAT(det::db_to_amplitude(decade_db), WithinRel(decade_amplitude, decibel_relative_bound));
    CHECK_THAT(det::amplitude_to_db(decade_amplitude), WithinRel(decade_db, decibel_relative_bound));
    constexpr std::array levels_db{-120.0, -60.0, -6.0, 6.0, 40.0, 120.0};

    for (const double level_db : levels_db)
    {
        CHECK_THAT(det::amplitude_to_db(det::db_to_amplitude(level_db)), WithinAbs(level_db, db_round_trip_bound));
    }
}

TEST_CASE("seconds_to_frames returns the frame counts the engine streams", "[det_math]")
{
    constexpr std::array frame_counts{std::uint64_t{1U}, std::uint64_t{signal_lab::engine_block_frames}, std::uint64_t{signal_lab::sample_rate_hz}};

    for (const std::uint64_t frames : frame_counts)
    {
        const double seconds = signal_lab::as_double(frames) / signal_lab::as_double(signal_lab::sample_rate_hz);
        CHECK(det::seconds_to_frames(seconds, signal_lab::sample_rate_hz) == frames);
    }

    CHECK(det::seconds_to_frames(0.0, signal_lab::sample_rate_hz) == 0U);
}

TEST_CASE("seconds_to_frames rounds half a frame to even", "[det_math]")
{
    for (const FrameRounding& rounding : half_frame_cases)
    {
        INFO("seconds " << rounding.seconds);
        CHECK(det::seconds_to_frames(rounding.seconds, signal_lab::sample_rate_hz) == rounding.frames);
    }
}

TEST_CASE("quantize_pcm24 rounds half to even and saturates", "[det_math]")
{
    // Half an LSB and one and a half LSBs, exact in float: 2^-24 and 3 * 2^-24.
    const auto half_lsb = signal_lab::to_float(0.5 / audio::pcm24_scale);
    const auto one_and_a_half_lsb = signal_lab::to_float(1.5 / audio::pcm24_scale);
    CHECK(det::quantize_pcm24(0.0F) == 0);
    CHECK(det::quantize_pcm24(half_lsb) == 0);
    CHECK(det::quantize_pcm24(one_and_a_half_lsb) == 2);
    CHECK(det::quantize_pcm24(-1.0F) == audio::pcm24_min);
    CHECK(det::quantize_pcm24(1.0F) == audio::pcm24_max);
}

TEST_CASE("quantize_pcm24 maps NaN to zero", "[det_math]")
{
    // Converting a NaN to an integer is undefined behaviour; the guard returns 0 first.
    CHECK(det::quantize_pcm24(std::numeric_limits<float>::quiet_NaN()) == 0);
    CHECK(det::quantize_pcm24(-std::numeric_limits<float>::quiet_NaN()) == 0);
}

TEST_CASE("Pcg32 streams are reproducible and separated by their key", "[det_math][rng]")
{
    const auto reference = first_draws(reference_key);
    CHECK(first_draws(reference_key) == reference);
    CHECK(first_draws({reference_key.seed + 1U, reference_key.family, reference_key.index}) != reference);
    CHECK(first_draws({reference_key.seed, reference_key.family + 1U, reference_key.index}) != reference);
    CHECK(first_draws({reference_key.seed, reference_key.family, reference_key.index + 1U}) != reference);
    // reseed() restarts a used generator at the beginning of its stream.
    det::Pcg32 generator(reference_key.seed, reference_key.family, reference_key.index);
    CHECK(generator.next() == reference.front());
    generator.reseed(reference_key.seed, reference_key.family, reference_key.index);
    CHECK(generator.next() == reference.front());
}

TEST_CASE("Pcg32 gives the known-answer outputs of a key", "[det_math][rng]")
{
    det::Pcg32 generator(known_answer_key.seed, known_answer_key.family, known_answer_key.index);

    for (const std::uint32_t expected : known_answer_draws)
    {
        CHECK(generator.next() == expected);
    }
}

TEST_CASE("Pcg32 uniform, gaussian and exponential give known-answer values", "[det_math][rng]")
{
    det::Pcg32 uniforms(unit_key.seed, unit_key.family, unit_key.index);

    for (const double expected : known_answer_uniforms)
    {
        CHECK(uniforms.uniform() == expected);
    }

    det::Pcg32 gaussian(unit_key.seed, unit_key.family, unit_key.index);
    CHECK(gaussian.gaussian() == known_answer_gaussian);
    det::Pcg32 exponential(unit_key.seed, unit_key.family, unit_key.index);
    CHECK_THAT(exponential.exponential(known_answer_exponential_mean), WithinRel(known_answer_exponential, exp_log_relative_bound));
}

TEST_CASE("Pcg32 variates stay in their documented ranges", "[det_math][rng]")
{
    det::Pcg32 generator(reference_key.seed, reference_key.family, reference_key.index);
    bool uniform_in_range = true;
    bool uniform_on_grid = true;
    bool gaussian_in_range = true;

    for (int draw = 0; draw < range_check_draws; ++draw)
    {
        const double uniform = generator.uniform();
        const double scaled = uniform * uniform_steps;
        const double gaussian = generator.gaussian();
        uniform_in_range = uniform_in_range && uniform >= 0.0 && uniform < 1.0;
        uniform_on_grid = uniform_on_grid && scaled == std::floor(scaled);
        gaussian_in_range = gaussian_in_range && std::fabs(gaussian) <= gaussian_bound;
    }

    CHECK(uniform_in_range);
    CHECK(uniform_on_grid);
    CHECK(gaussian_in_range);
}

TEST_CASE("StreamDigest is independent of block boundaries", "[det_math][digest]")
{
    constexpr std::array samples{0.0F, 0.25F, -0.5F, 0.75F, -1.0F, 0.125F};
    const std::span all{samples};
    const std::size_t split = samples.size() / 2U;
    det::StreamDigest whole;
    whole.update(all.data(), all.size());
    det::StreamDigest blocks;
    blocks.update(all.first(split).data(), split);
    blocks.update(all.subspan(split).data(), all.size() - split);
    CHECK(blocks.value() == whole.value());
    // reset() returns to the digest of an empty stream.
    const det::StreamDigest empty;
    blocks.reset();
    CHECK(blocks.value() == empty.value());
    CHECK(whole.value() != empty.value());
}

TEST_CASE("StreamDigest gives the known-answer digests of both overloads", "[det_math][digest]")
{
    det::StreamDigest floats;
    floats.reset();
    floats.update(digest_floats.data(), digest_floats.size());
    CHECK(floats.value() == digest_floats_value);
    det::StreamDigest pcm16;
    pcm16.reset();
    pcm16.update(digest_pcm16.data(), digest_pcm16.size());
    CHECK(pcm16.value() == digest_pcm16_value);
}
