#pragma once

#include "core/status.hpp"

#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <span>

namespace m110
{

// Precision is a build-time choice. Real is the one scalar type the whole modem
// is parameterized on; the default is single precision (the ship configuration
// for every current target - the M33/U545 has no hardware double, and it is a
// needless slowdown on the M7s). Define M110_PRECISION_DOUBLE=1 to build the
// double variant (a numerical oracle; it additionally requires the float-literal
// sweep to compile clean under -Wdouble-promotion - deferred). This is a
// build-time preprocessor gate; see the "Build options" table in core/README.md.
#if defined(M110_PRECISION_DOUBLE) && M110_PRECISION_DOUBLE
using Real = double;
#else
using Real = float;
#endif

using Sample = Real;
using IQSample = std::complex<Real>;

// Precision-parametric scalar real math. The complex helpers (std::polar / abs /
// arg / conj / norm) are already templates on the value type and so parameterize
// for free; only the scalar-real transcendentals need a dispatch layer, because
// std::sqrt/sin/... on a bare Real would risk silent double promotion. real_ops
// binds each to its EXPLICIT single/double twin, so a float build emits
// sqrtf/sinf (no promotion) and a double build emits sqrt/sin from one source.
// This is also the seam where a future fixed-point Real (Q15/Q31) would back
// sin/sqrt with the M33 CORDIC (WBS 5.5 U545 acceleration).
template <class T>
struct real_ops;

template <>
struct real_ops<float>
{
    [[nodiscard]] static float sqrt(float x) noexcept { return std::sqrtf(x); }
    [[nodiscard]] static float sin(float x) noexcept { return std::sinf(x); }
    [[nodiscard]] static float cos(float x) noexcept { return std::cosf(x); }
    [[nodiscard]] static float atan2(float y, float x) noexcept { return std::atan2f(y, x); }
    [[nodiscard]] static float fabs(float x) noexcept { return std::fabsf(x); }
    [[nodiscard]] static float exp(float x) noexcept { return std::expf(x); }
    [[nodiscard]] static float log(float x) noexcept { return std::logf(x); }
    [[nodiscard]] static float pow(float base, float exponent) noexcept { return std::powf(base, exponent); }
    [[nodiscard]] static float hypot(float x, float y) noexcept { return std::hypotf(x, y); }
    [[nodiscard]] static float floor(float x) noexcept { return std::floorf(x); }
    [[nodiscard]] static float ceil(float x) noexcept { return std::ceilf(x); }
    [[nodiscard]] static float round(float x) noexcept { return std::roundf(x); }
    [[nodiscard]] static float fmin(float a, float b) noexcept { return std::fminf(a, b); }
    [[nodiscard]] static float fmax(float a, float b) noexcept { return std::fmaxf(a, b); }
    [[nodiscard]] static float fmod(float a, float b) noexcept { return std::fmodf(a, b); }
};

template <>
struct real_ops<double>
{
    [[nodiscard]] static double sqrt(double x) noexcept { return std::sqrt(x); }
    [[nodiscard]] static double sin(double x) noexcept { return std::sin(x); }
    [[nodiscard]] static double cos(double x) noexcept { return std::cos(x); }
    [[nodiscard]] static double atan2(double y, double x) noexcept { return std::atan2(y, x); }
    [[nodiscard]] static double fabs(double x) noexcept { return std::fabs(x); }
    [[nodiscard]] static double exp(double x) noexcept { return std::exp(x); }
    [[nodiscard]] static double log(double x) noexcept { return std::log(x); }
    [[nodiscard]] static double pow(double base, double exponent) noexcept { return std::pow(base, exponent); }
    [[nodiscard]] static double hypot(double x, double y) noexcept { return std::hypot(x, y); }
    [[nodiscard]] static double floor(double x) noexcept { return std::floor(x); }
    [[nodiscard]] static double ceil(double x) noexcept { return std::ceil(x); }
    [[nodiscard]] static double round(double x) noexcept { return std::round(x); }
    [[nodiscard]] static double fmin(double a, double b) noexcept { return std::fmin(a, b); }
    [[nodiscard]] static double fmax(double a, double b) noexcept { return std::fmax(a, b); }
    [[nodiscard]] static double fmod(double a, double b) noexcept { return std::fmod(a, b); }
};

// The modem's scalar-real math entry point: rmath::sqrt(x), rmath::sin(x), ...
using rmath = real_ops<Real>;

using SampleSpan = std::span<const Sample>;
using MutableSampleSpan = std::span<Sample>;
using IQSampleSpan = std::span<const IQSample>;
using MutableIQSampleSpan = std::span<IQSample>;
using BitSpan = std::span<const std::uint8_t>;
using MutableBitSpan = std::span<std::uint8_t>;

enum class BlockFlag : std::uint32_t
{
    none = 0,
    discontinuity = 1U << 0U,
    clipping = 1U << 1U,
    gain_transition = 1U << 2U,
    end_of_stream = 1U << 3U
};

[[nodiscard]] constexpr BlockFlag operator|(BlockFlag lhs, BlockFlag rhs) noexcept
{
    return static_cast<BlockFlag>(static_cast<std::uint32_t>(lhs) | static_cast<std::uint32_t>(rhs));
}

[[nodiscard]] constexpr BlockFlag operator&(BlockFlag lhs, BlockFlag rhs) noexcept
{
    return static_cast<BlockFlag>(static_cast<std::uint32_t>(lhs) & static_cast<std::uint32_t>(rhs));
}

[[nodiscard]] constexpr bool has_flag(BlockFlag flags, BlockFlag flag) noexcept
{
    return (flags & flag) != BlockFlag::none;
}

struct BasebandBlockView
{
    IQSampleSpan samples{};
    std::uint32_t sample_rate_hz{};
    std::uint64_t first_sample_index{};
    std::uint32_t stream_id{};
    std::uint32_t coherence_group{};
    float gain_db{};
    BlockFlag flags{BlockFlag::none};
};

struct MutableBasebandBlockView
{
    MutableIQSampleSpan samples{};
    std::uint32_t sample_rate_hz{};
    std::uint64_t first_sample_index{};
    std::uint32_t stream_id{};
    std::uint32_t coherence_group{};
    float gain_db{};
    BlockFlag flags{BlockFlag::none};

    [[nodiscard]] BasebandBlockView as_const() const noexcept
    {
        return {samples, sample_rate_hz, first_sample_index, stream_id, coherence_group, gain_db, flags};
    }
};

struct StreamContinuity
{
    std::uint32_t stream_id{};
    std::uint64_t expected_first_sample{};
    bool initialized{};
};

[[nodiscard]] Status validate_block(const BasebandBlockView& block) noexcept;
[[nodiscard]] Status validate_and_advance(const BasebandBlockView& block, StreamContinuity& continuity) noexcept;

} // namespace m110
