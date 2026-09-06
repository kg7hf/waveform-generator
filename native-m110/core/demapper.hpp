#pragma once

#include "core/status.hpp"
#include "core/types.hpp"

#include <cstdint>
#include <span>

// =============================================================================
// demapper.hpp - 8-PSK mapping + hard/soft/turbo demappers
// =============================================================================
// psk8_symbol maps a tribit to the constellation; the demappers turn a received
// symbol into per-bit soft LLRs (+ favors 0). psk8_soft_demapper_apriori is the
// turbo-loop demapper (folds in decoder a-priori, returns extrinsic independent of
// each bit's own prior).
//
// These soft LLRs are the SISO decoder's input (via the deinterleaver, in encoder
// order). The demapper ALWAYS emits max-log values; it is the SISO trellis - not
// this file - that optionally recombines them with exact log-MAP (max*) when the
// turbo engine's log_map lever is set (default OFF, opt-in). The turbo path passes
// noise_variance as the equalizer's effective MMSE variance gain*(1-gain) (the CSI
// contract in turbo_equalizer.cpp): as gain -> 1 that variance shrinks toward zero
// and the emitted LLRs grow over-confident, which is why exact log-MAP is
// mode-dependent (it helps the repetition/fast-fade modes but hurts the
// over-confident slow-fade 600/1200 case). Teaching walkthrough:
//   core/demapper-and-deinterleaver-explainer.md
//   turbo levers (log_map / probe_noise_tracking / extrinsic_damping, all opt-in,
//   defaults unchanged) -> core/turbo-equalizer-and-burst-decoder-explainer.md
// =============================================================================

namespace m110
{

[[nodiscard]] IQSample psk8_symbol(std::uint8_t tribit) noexcept;
[[nodiscard]] std::uint8_t psk8_hard_demapper(IQSample symbol) noexcept;

// Produces max-log soft values in source-bit order. Positive values favor 0
// and negative values favor 1.
[[nodiscard]] Status psk8_soft_demapper(IQSample symbol, std::uint8_t source_bits, std::span<float> soft_bits) noexcept;

// WBS 6.16 S4 - a-priori-aware max-log demapper for the turbo equalizer.
// symbol_estimate is the equalizer output already derotated by the
// scrambler (exactly as extract_soft_data derotates), modelled as
// symbol_gain * s(v) + w with w complex Gaussian of total variance
// noise_variance, s(v) = psk8_symbol(mapped_tribit(v)) over the 2^source_bits
// constellation points of the body mapping (Table XI/XII/XIII widths).
// apriori holds source_bits LLRs (positive favors 0) and extrinsic receives
// source_bits outputs. metric(v) = -|z - gain*s(v)|^2 / noise_variance
// + 0.5 * sum over bits (bit == 0 ? +La : -La); the extrinsic of bit i is
// max over v with bit i = 0 minus max over v with bit i = 1 of the metric
// WITHOUT bit i's own a-priori term, which equals the posterior LLR minus
// La_i in max-log arithmetic and is bit-exactly independent of La_i. With
// zero a-priori, gain 1 and noise_variance 1 it reduces to
// psk8_soft_demapper's values exactly.
[[nodiscard]] Status psk8_soft_demapper_apriori(IQSample symbol_estimate, float symbol_gain, float noise_variance, std::uint8_t source_bits, std::span<const float> apriori,
        std::span<float> extrinsic) noexcept;

} // namespace m110
