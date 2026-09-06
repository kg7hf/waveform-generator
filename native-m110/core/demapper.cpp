/*
================================================================================
demapper.cpp - 8-PSK symbol mapping and soft/hard demapping
================================================================================

WHAT THIS FILE IS
-----------------
The bridge from equalized complex SYMBOLS to BIT EVIDENCE. It maps tribits to the
8-PSK constellation (transmit) and demaps a received symbol back into soft bits
(receive). Soft = a log-likelihood ratio per bit (+ favors 0, - favors 1, magnitude
= confidence), which the FEC/SISO decoder and the turbo loop need instead of hard
0/1 guesses.

  INPUT  : one equalized symbol z (+ gain + noise variance for the turbo demapper).
  OUTPUT : per-bit soft LLRs in source-bit order.
  DEPENDS: scrambler.hpp (modified_gray_decode - the body bit->tribit sub-mapping).

WHAT LIVES HERE
---------------
  psk8_symbol()                tribit -> constellation point e^{j*tribit*pi/4} (table).
  psk8_hard_demapper()         nearest point (hard decision, e.g. for feedback).
  psk8_soft_demapper()         max-log LLRs, no prior (min-distance form).
  psk8_soft_demapper_apriori() the TURBO demapper: folds in decoder a-priori LLRs and
                               returns EXTRINSIC LLRs, EXCLUDING each bit's own prior.

SIGN CONVENTION (load-bearing)
------------------------------
  Positive soft value favors bit 0, negative favors bit 1. Both demappers agree by
  construction (one_cost - zero_cost / zero_best - one_best). The decoder assumes it.

DOWNSTREAM + CSI CONTRACT
-------------------------
  These soft LLRs go to the deinterleaver (encoder order) and then the SISO/FEC
  decoder. The turbo demapper is handed noise_variance = the equalizer's effective
  MMSE variance gain*(1-gain) (the CSI contract in turbo_equalizer.cpp): as gain -> 1
  it shrinks toward zero, so the emitted LLRs become over-confident. The demapper
  itself is ALWAYS max-log; it is the SISO trellis that optionally recombines these
  LLRs with exact log-MAP (max*) under the turbo log_map lever (default off, opt-in),
  which is why that lever's benefit is mode-dependent (helps repetition/fast-fade,
  hurts the over-confident slow-fade 600/1200 modes; defaults unchanged per the owner
  decision of 2026-09-06). Turbo levers reference:
  core/turbo-equalizer-and-burst-decoder-explainer.md.

WORKED EXAMPLE / DEEP DIVE
--------------------------
    core/demapper-and-deinterleaver-explainer.md
================================================================================
*/

#include "core/demapper.hpp"

#include "core/scrambler.hpp"

#include <array>
#include <cmath>
#include <complex>
#include <cstdint>
#include <limits>

namespace m110
{

constexpr float pi_over_four = 0.78539816339744830962F;

IQSample psk8_symbol(std::uint8_t tribit) noexcept
{
    // The 8-PSK constellation has only 8 possible outputs. Precompute them once
    // so the per-symbol hot path (demap derotation, reference generation) is a
    // table read, not a software sinf/cosf on cores without hardware trig (the
    // Cortex-M33). The table is filled with the same std::polar expression, so
    // every value is bit-identical to the previous per-call computation - the
    // decode is unchanged; only the M33 cost drops.
    static const std::array<IQSample, 8U> constellation = []() noexcept
    {
        std::array<IQSample, 8U> table{};

        for (std::uint8_t index = 0U; index < 8U; ++index)
        {
            table[index] = std::polar(1.0F, static_cast<float>(index) * pi_over_four);
        }

        return table;
    }();

    return constellation[tribit & 7U];
}

std::uint8_t psk8_hard_demapper(IQSample symbol) noexcept
{
    constexpr float two_pi = 6.2831853071795864769F;
    auto phase = std::arg(symbol);

    if (phase < 0.0F)
    {
        phase += two_pi;
    }

    return static_cast<std::uint8_t>(std::lround(phase / pi_over_four)) & 7U;
}

// -----------------------------------------------------------------------------
// psk8_soft_demapper  (max-log soft demap, no prior)
// -----------------------------------------------------------------------------
// 50K view: One LLR per source bit from a received symbol - the FEC decoder's input
//   when there is no turbo feedback.
// Detailed view: cost(v) = |symbol - psk8(v)|^2 over the 2^source_bits candidates
//   (modified-Gray mapped). For each bit LLR = min-cost(bit=1) - min-cost(bit=0), so
//   positive favors 0. Max-log-MAP (min-distance, not full log-sum-exp).
// 5th-grade view: For each bit, compare the nearest 0-spot with the nearest 1-spot;
//   the gap is your confidence.
// -----------------------------------------------------------------------------
Status psk8_soft_demapper(IQSample symbol, std::uint8_t source_bits, std::span<float> soft_bits) noexcept
{
    if (source_bits == 0U || source_bits > 3U || soft_bits.size() < source_bits)
    {
        return {StatusCode::invalid_argument, "invalid 8PSK soft-demapper request"};
    }

    const auto candidates = static_cast<std::uint8_t>(1U << source_bits);
    std::array<float, 8> costs{};

    for (std::uint8_t value = 0U; value < candidates; ++value)
    {
        auto tribit = modified_gray_decode(value, source_bits);

        if (source_bits == 2U)
        {
            tribit = static_cast<std::uint8_t>(tribit << 1U);
        }

        costs[value] = std::norm(symbol - psk8_symbol(tribit));
    }

    for (std::uint8_t bit = 0U; bit < source_bits; ++bit)
    {
        auto zero_cost = std::numeric_limits<float>::infinity();
        auto one_cost = std::numeric_limits<float>::infinity();

        for (std::uint8_t value = 0U; value < candidates; ++value)
        {
            const bool bit_value = ((value >> (source_bits - bit - 1U)) & 1U) != 0U;
            auto& target = bit_value ? one_cost : zero_cost;

            if (costs[value] < target)
            {
                target = costs[value];
            }
        }

        soft_bits[bit] = one_cost - zero_cost;
    }

    return Status::success();
}

// -----------------------------------------------------------------------------
// psk8_soft_demapper_apriori  (the TURBO demapper)
// -----------------------------------------------------------------------------
// 50K view: Max-log demap that folds in decoder a-priori LLRs and returns EXTRINSIC
//   LLRs, excluding each bit's own prior.
// Detailed view: channel(v) = -|z - gain*s(v)|^2 / noise_variance (a real likelihood
//   using the equalizer's gain + noise). metric(v) = channel(v) + sum_{other != bit}
//   (+/- 0.5*La_other); extrinsic(bit) = max_{v:bit=0} metric - max_{v:bit=1} metric.
//   The bit's OWN La is left out of the sum (not subtracted after) so a saturated La
//   (+-1e6 for trellis-pinned bits) cannot swamp the channel term in float
//   cancellation - the extrinsic is bit-exactly independent of its own input.
// CSI note: the turbo equalizer passes noise_variance = the effective MMSE variance
//   gain*(1-gain) and symbol_gain = gain. This demapper stays max-log and is robust
//   to that scale, but the LLRs it emits feed the SISO decoder, and when gain -> 1
//   the variance -> 0 so those LLRs turn over-confident. That miscalibration is why
//   the SISO trellis's exact log-MAP (max*) option - the turbo log_map lever, default
//   off / opt-in - helps the repetition and fast-fade modes but hurts the
//   over-confident slow-fade 600/1200 case; see
//   core/turbo-equalizer-and-burst-decoder-explainer.md.
// 5th-grade view: Same dartboard confidence, plus the teacher's hints about the OTHER
//   bits - while ignoring the teacher's hint about THIS bit so you don't echo it back.
// -----------------------------------------------------------------------------
Status psk8_soft_demapper_apriori(IQSample symbol_estimate, float symbol_gain, float noise_variance, std::uint8_t source_bits, std::span<const float> apriori,
                                  std::span<float> extrinsic) noexcept
{
    if (source_bits == 0U || source_bits > 3U || apriori.size() < source_bits || extrinsic.size() < source_bits || !(noise_variance > 0.0F))
    {
        return {StatusCode::invalid_argument, "invalid a-priori 8PSK soft-demapper request"};
    }

    const auto candidates = static_cast<std::uint8_t>(1U << source_bits);
    // Channel term per constellation point. The body mapping is the same
    // modified-Gray walk psk8_soft_demapper uses (1 bit -> 0/4, 2 bits ->
    // gray << 1, 3 bits -> gray), so both demappers rank the same points.
    std::array<float, 8> channel{};
    const auto inverse_variance = 1.0F / noise_variance;

    for (std::uint8_t value = 0U; value < candidates; ++value)
    {
        auto tribit = modified_gray_decode(value, source_bits);

        if (source_bits == 2U)
        {
            tribit = static_cast<std::uint8_t>(tribit << 1U);
        }

        channel[value] = -std::norm(symbol_estimate - symbol_gain * psk8_symbol(tribit)) * inverse_variance;
    }

    for (std::uint8_t bit = 0U; bit < source_bits; ++bit)
    {
        // The bit's own a-priori is left out of the metric rather than
        // subtracted afterwards: a saturated La (the decoder reports
        // +-1e6 for trellis-pinned bits) would otherwise swallow the channel
        // difference in float cancellation, and the exclusion form is what
        // makes the extrinsic bit-exactly independent of its own input.
        auto zero_best = -std::numeric_limits<float>::infinity();
        auto one_best = -std::numeric_limits<float>::infinity();

        for (std::uint8_t value = 0U; value < candidates; ++value)
        {
            auto metric = channel[value];

            for (std::uint8_t other = 0U; other < source_bits; ++other)
            {
                if (other == bit)
                {
                    continue;
                }

                const bool other_value = ((value >> (source_bits - other - 1U)) & 1U) != 0U;
                metric += other_value ? -0.5F * apriori[other] : 0.5F * apriori[other];
            }

            const bool bit_value = ((value >> (source_bits - bit - 1U)) & 1U) != 0U;
            auto& target = bit_value ? one_best : zero_best;

            if (metric > target)
            {
                target = metric;
            }
        }

        extrinsic[bit] = zero_best - one_best;
    }

    return Status::success();
}

} // namespace m110
