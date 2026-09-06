#pragma once

#include "core/status.hpp"
#include "core/types.hpp"

#include <cstddef>
#include <cstdint>
#include <span>

// =============================================================================
// fec.hpp - K=7 rate-1/2 convolutional encoder + Viterbi decoders (hard output)
// =============================================================================
// The FEC rulebook: ConvolutionalEncoderK7 (generators 133/171 octal), the encode
// variants (rate 1/2, repeat xN, tail-biting punctured 3/4), the whole-buffer
// viterbi_decode_rate_half, the continuous StreamingViterbiK7 (fixed traceback
// window), and the tail-biting 3/4 decoder. Soft metrics: T1 before T2, + favors 0.
// The SOFT (turbo) twin is siso_decoder.hpp. Teaching walkthrough:
//   core/fec-and-siso-decoder-explainer.md
// =============================================================================

namespace m110
{

struct EncodedPair
{
    std::uint8_t t1{};
    std::uint8_t t2{};
};

class ConvolutionalEncoderK7
{
public:
    explicit constexpr ConvolutionalEncoderK7(std::uint8_t initial_state = 0U) noexcept : state_
    {
        static_cast<std::uint8_t>(initial_state & 0x3FU)
    } {}

    [[nodiscard]] EncodedPair push(std::uint8_t bit) noexcept;

    [[nodiscard]] std::uint8_t state() const noexcept
    {
        return state_;
    }

    void reset(std::uint8_t state = 0U) noexcept
    {
        state_ = static_cast<std::uint8_t>(state & 0x3FU);
    }

private:
    std::uint8_t state_{};
};

[[nodiscard]] Result<std::size_t> encode_rate_half(BitSpan input, MutableBitSpan output, std::uint8_t initial_state = 0U) noexcept;

[[nodiscard]] Result<std::size_t> encode_repeated_pairs(BitSpan input, MutableBitSpan output, std::uint8_t pair_repetitions, std::uint8_t initial_state = 0U) noexcept;

[[nodiscard]] Result<std::size_t> encode_tail_biting_punctured_3_4(BitSpan input, MutableBitSpan output) noexcept;

// Positive soft values mean bit 0; negative values mean bit 1.
[[nodiscard]] Status viterbi_decode_rate_half(std::span<const float> soft_bits, MutableBitSpan decoded, MutableBitSpan survivor_scratch, std::uint8_t initial_state = 0U,
        bool require_zero_final_state = false) noexcept;

// Streaming K=7 rate-1/2 Viterbi decoder with a fixed traceback window,
// replacing the whole-buffer survivor table for continuous receive. It keeps
// every convention of viterbi_decode_rate_half - positive soft metrics favor
// bit 0, T1 before T2, additive minimized cost, states with the newest input
// bit in bit 0, hard-pinned initial state, add/compare order and strict-less
// tie breaks - so on a merged trellis both decoders emit identical bits. One
// survivor word stores the 64 predecessor decisions of one step; the caller
// provides the window (one std::uint64_t per traceback step) so the depth is
// a workspace decision, not an allocation.
class StreamingViterbiK7
{
  public:
    static constexpr std::size_t state_count = 64U;
    static constexpr std::size_t minimum_traceback_depth = 32U;
    static constexpr std::size_t default_traceback_depth = 96U;

    [[nodiscard]] Status initialize(std::span<std::uint64_t> survivor_window) noexcept;
    void restart(std::uint8_t initial_state) noexcept;

    // Consumes one combined rate-1/2 metric pair and emits at most one stable
    // bit (the bit leaving the traceback window). Bits emerge in encoder input
    // order, delayed by the window depth.
    [[nodiscard]] std::size_t update(std::int32_t soft_t1, std::int32_t soft_t2, MutableBitSpan stable_bits) noexcept;

    // Emits the bits still inside the traceback window from the current best
    // path, oldest first. stable_bits must hold pending_bits() entries.
    [[nodiscard]] std::size_t flush(MutableBitSpan stable_bits) noexcept;

    [[nodiscard]] std::size_t pending_bits() const noexcept
    {
        return pending_steps_;
    }

    [[nodiscard]] std::size_t traceback_depth() const noexcept
    {
        return survivors_.size();
    }

  private:
    [[nodiscard]] std::uint8_t best_state() const noexcept;

    std::span<std::uint64_t> survivors_{};
    std::array<std::int32_t, state_count> metrics_{};
    std::array<std::int32_t, state_count> next_metrics_{};
    std::array<std::uint8_t, 2U * state_count> branch_pairs_{};
    std::size_t newest_step_{};
    std::size_t pending_steps_{};
    bool initialized_{};
};

[[nodiscard]] Status viterbi_decode_tail_biting_3_4(std::span<const float> punctured_soft_bits, MutableBitSpan decoded, std::span<float> depunctured_scratch, MutableBitSpan survivor_scratch) noexcept;

} // namespace m110
