#pragma once

#include "core/status.hpp"
#include "core/types.hpp"
#include "core/waveform.hpp"

#include <cstddef>
#include <cstdint>
#include <span>

// =============================================================================
// interleaver.hpp - body matrix interleaver + high-rate interleaver (hard + soft)
// =============================================================================
// Reorders coded bits so a channel fade becomes scattered (not bursty) coded-bit
// errors. body_* is the MIL-STD-188-110B rows x columns matrix (load down columns,
// fetch across rows); high_rate_* is a multiplicative permutation. Deinterleave is
// the exact inverse of interleave; the soft variants carry the turbo loop's LLRs
// both directions (interleave_soft(deinterleave_soft(x)) == x) - deinterleave_soft
// hands the demapper's soft LLRs to the SISO decoder in encoder order, and
// interleave_soft returns the decoder's a-priori to the demapper in channel order.
// This stage only REORDERS those LLRs; it never rescales them, so the demapper's
// effective-variance (gain*(1-gain)) calibration and the SISO log_map lever (see the
// turbo reference) are untouched here. Teaching walkthrough:
//   core/demapper-and-deinterleaver-explainer.md
//   turbo levers -> core/turbo-equalizer-and-burst-decoder-explainer.md
// =============================================================================

namespace m110
{

[[nodiscard]] Status body_interleave(BitSpan input, MutableBitSpan scratch, MutableBitSpan output, const InterleaverSpec& spec) noexcept;
[[nodiscard]] Status body_deinterleave(BitSpan input, MutableBitSpan scratch, MutableBitSpan output, const InterleaverSpec& spec) noexcept;
[[nodiscard]] Status body_deinterleave_soft(std::span<const float> input, std::span<float> scratch, std::span<float> output, const InterleaverSpec& spec) noexcept;
// Exact inverse of body_deinterleave_soft (the transmit load/fetch walk on
// soft values): a turbo equalizer re-interleaves the decoder's a-priori
// back into channel order. interleave_soft(deinterleave_soft(x)) == x.
[[nodiscard]] Status body_interleave_soft(std::span<const float> input, std::span<float> scratch, std::span<float> output, const InterleaverSpec& spec) noexcept;

[[nodiscard]] Status high_rate_interleave(BitSpan input, MutableBitSpan scratch, MutableBitSpan output, const InterleaverSpec& spec) noexcept;
[[nodiscard]] Status high_rate_deinterleave(BitSpan input, MutableBitSpan scratch, MutableBitSpan output, const InterleaverSpec& spec) noexcept;
[[nodiscard]] Status high_rate_deinterleave_soft(std::span<const float> input, std::span<float> scratch, std::span<float> output, const InterleaverSpec& spec) noexcept;

[[nodiscard]] Result<std::size_t> body_load_address(std::size_t input_index, const InterleaverSpec& spec) noexcept;
[[nodiscard]] Result<std::size_t> body_fetch_address(std::size_t output_index, const InterleaverSpec& spec) noexcept;
[[nodiscard]] Result<std::size_t> high_rate_address(std::size_t index, const InterleaverSpec& spec) noexcept;

} // namespace m110
