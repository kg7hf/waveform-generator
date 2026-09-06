/*
================================================================================
fec.cpp - forward error correction: K=7 convolutional encoder + Viterbi decoders
================================================================================

WHAT THIS FILE IS
-----------------
The error-correction rulebook. The transmitter runs data through a K=7 rate-1/2
convolutional encoder (generators 133/171 octal), so the coded bits are correlated
in a known way; the receiver's Viterbi decoders exploit that structure to recover
the exact transmitted bits from a corrupted soft-bit stream. HARD output here; the
SOFT (turbo) engine is siso_decoder.cpp.

  INPUT  : (encode) data bits; (decode) soft LLRs, T1 before T2, + favors bit 0.
  OUTPUT : (encode) coded bits; (decode) recovered information bits.
  DEPENDS: types.hpp / status.hpp only.

THE CODE
--------
  K=7 => 64-state trellis. ConvolutionalEncoderK7::push shifts one input bit in and
  emits EncodedPair {t1,t2}. Generators 0x6D/0x4F are octal 133/171 with the newest
  input bit in bit 0.

WHAT LIVES HERE
---------------
  encode_rate_half / encode_repeated_pairs / encode_tail_biting_punctured_3_4
      the three transmit encodings (plain, repeat xN for low rates, punctured 3/4).
  viterbi_decode_rate_half (decode_with_end_state)
      whole-buffer maximum-likelihood decode: add-compare-select + traceback.
  StreamingViterbiK7
      continuous decode with a FIXED traceback window (bits emerge delayed by the
      window depth) - the block-by-block streaming decode mechanism.
  viterbi_decode_tail_biting_3_4
      depuncture, then try all 64 tail-biting start states, keep the best.

CONVENTIONS (shared with siso_decoder + demapper + interleaver)
---------------------------------------------------------------
  T1 before T2; positive soft metric favors bit 0; newest input bit in state bit 0;
  additive minimized cost. StreamingViterbiK7 keeps every one of these so it is
  bit-identical to the batch decoder on a merged trellis.

WORKED EXAMPLE / DEEP DIVE
--------------------------
    core/fec-and-siso-decoder-explainer.md
================================================================================
*/

#include "core/fec.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>

namespace m110
{
// MIL-STD-188-110B K=7 generators, T1 first. The standard's octal
// polynomials 133 and 171 become 0x6D and 0x4F when the newest input bit is
// stored in bit 0, as ConvolutionalEncoderK7 does below.
constexpr std::uint8_t polynomial_t1 = 0x6DU;
constexpr std::uint8_t polynomial_t2 = 0x4FU;
constexpr std::size_t state_count = 64U;
// MIL-STD-188-110C Appendix D, Table D-XLII 3/4-rate puncturing mask, applied
// to the serialized T1,T2 stream in that order.
constexpr std::array<std::uint8_t, 6> puncture_mask{1U, 1U, 1U, 0U, 0U, 1U};

static constexpr std::uint8_t parity(std::uint8_t value) noexcept
{
    return static_cast<std::uint8_t>(std::popcount(value) & 1U);
}

// Additive (minimized) branch metric for one trellis edge: a coded bit the branch
// expects to be 0 subtracts its LLR, a bit it expects to be 1 adds it, so the
// expected pair that agrees with the soft signs earns the lowest cost (positive LLR
// favors 0). This is the hard-decoder mirror of the SISO gamma, which shifts and
// halves the same +-L terms into a maximized log-domain score.
static float branch_cost(float soft_t1, float soft_t2, const EncodedPair expected) noexcept
{
    return (expected.t1 == 0U ? -soft_t1 : soft_t1) + (expected.t2 == 0U ? -soft_t2 : soft_t2);
}

// -----------------------------------------------------------------------------
// decode_with_end_state  (whole-buffer Viterbi core)
// -----------------------------------------------------------------------------
// 50K view: Find the maximum-likelihood bit sequence through the 64-state trellis.
// Detailed view: For each step, add-compare-select: extend all 64 states by input 0
//   and 1, add branch_cost (+soft if the expected coded bit is 1, -soft if 0), keep
//   the lower-cost predecessor per next state, record the survivor. Then pick the
//   terminal state (0 if required, else min-metric) and TRACE BACK the survivors to
//   emit bits. Returns the winning path metric (used by the tail-biting search).
// 5th-grade view: Walk the maze one step at a time keeping the cheapest way into
//   each room; at the end, backtrack the cheapest route and read off the digits.
// -----------------------------------------------------------------------------
static Result<float> decode_with_end_state(std::span<const float> soft_bits, MutableBitSpan decoded, MutableBitSpan survivor_scratch, std::uint8_t initial_state, int required_final_state) noexcept
{
    const auto steps = soft_bits.size() / 2U;

    if ((soft_bits.size() & 1U) != 0U || decoded.size() != steps || survivor_scratch.size() < steps * state_count)
    {
        return Status{StatusCode::buffer_too_small, "Viterbi buffers do not match trellis length"};
    }

    constexpr float infinity = std::numeric_limits<float>::infinity();
    std::array<float, state_count> metrics{};
    std::array<float, state_count> next_metrics{};
    metrics.fill(infinity);
    metrics[initial_state & 0x3FU] = 0.0F;

    for (std::size_t step = 0U; step < steps; ++step)
    {
        next_metrics.fill(infinity);

        for (std::size_t previous = 0U; previous < state_count; ++previous)
        {
            if (metrics[previous] == infinity)
            {
                continue;
            }

            for (std::uint8_t bit = 0U; bit < 2U; ++bit)
            {
                ConvolutionalEncoderK7 encoder(static_cast<std::uint8_t>(previous));
                const auto expected = encoder.push(bit);
                const auto next = encoder.state();
                const auto candidate = metrics[previous] + branch_cost(soft_bits[step * 2U], soft_bits[step * 2U + 1U], expected);

                if (candidate < next_metrics[next])
                {
                    next_metrics[next] = candidate;
                    survivor_scratch[step * state_count + next] = static_cast<std::uint8_t>(previous);
                }
            }
        }

        metrics = next_metrics;
    }

    std::uint8_t final_state = 0U;

    if (required_final_state >= 0)
    {
        final_state = static_cast<std::uint8_t>(required_final_state);
    }
    else
    {
        for (std::uint8_t state = 1U; state < state_count; ++state)
        {
            if (metrics[state] < metrics[final_state])
            {
                final_state = state;
            }
        }
    }

    if (metrics[final_state] == infinity)
    {
        return Status{StatusCode::invalid_argument, "no valid Viterbi path"};
    }

    auto state = final_state;

    for (std::size_t step = steps; step > 0U; --step)
    {
        decoded[step - 1U] = static_cast<std::uint8_t>(state & 1U);
        state = survivor_scratch[(step - 1U) * state_count + state];
    }

    return metrics[final_state];
}

// -----------------------------------------------------------------------------
// ConvolutionalEncoderK7::push  (the code core - one input bit -> two coded bits)
// -----------------------------------------------------------------------------
// 50K view: Shift one input bit into the 6-bit register and emit the rate-1/2 coded
//   pair (t1, t2).
// Detailed view: register = (state<<1 | bit) & 0x7F; state' = register & 0x3F;
//   t1 = parity(register & 0x6D), t2 = parity(register & 0x4F) - generators 133/171
//   octal (bit-reversed, newest input in bit 0). Each output is the XOR of the
//   register bits its polynomial selects.
// 5th-grade view: Slide the new digit into a little window and announce two check
//   sounds computed from the window.
// -----------------------------------------------------------------------------
EncodedPair ConvolutionalEncoderK7::push(std::uint8_t bit) noexcept
{
    const auto register_value = static_cast<std::uint8_t>(((state_ << 1U) | (bit & 1U)) & 0x7FU);
    state_ = static_cast<std::uint8_t>(register_value & 0x3FU);
    return {parity(static_cast<std::uint8_t>(register_value & polynomial_t1)), parity(static_cast<std::uint8_t>(register_value & polynomial_t2))};
}

Result<std::size_t> encode_rate_half(BitSpan input, MutableBitSpan output, std::uint8_t initial_state) noexcept
{
    if (output.size() < input.size() * 2U)
    {
        return Status{StatusCode::buffer_too_small, "FEC output too small"};
    }

    ConvolutionalEncoderK7 encoder(initial_state);
    std::size_t written = 0U;

    for (const auto bit : input)
    {
        const auto pair = encoder.push(bit);
        output[written++] = pair.t1;
        output[written++] = pair.t2;
    }

    return written;
}

Result<std::size_t> encode_repeated_pairs(BitSpan input, MutableBitSpan output, std::uint8_t pair_repetitions, std::uint8_t initial_state) noexcept
{
    if (pair_repetitions == 0U || output.size() < input.size() * 2U * pair_repetitions)
    {
        return Status{StatusCode::buffer_too_small, "repeated FEC output too small"};
    }

    ConvolutionalEncoderK7 encoder(initial_state);
    std::size_t written = 0U;

    for (const auto bit : input)
    {
        const auto pair = encoder.push(bit);

        for (std::uint8_t repeat = 0U; repeat < pair_repetitions; ++repeat)
        {
            output[written++] = pair.t1;
            output[written++] = pair.t2;
        }
    }

    return written;
}

// -----------------------------------------------------------------------------
// encode_tail_biting_punctured_3_4
// -----------------------------------------------------------------------------
// 50K view: The high-rate transmit encoding: tail-biting (no flush tail) + puncture
//   to rate 3/4.
// Detailed view: Warm the register with input[0..5] (no output), encode input
//   [6..N-1], then wrap to encode input[0..5] - so the end state equals the start
//   state (tail-biting). Keep only 4 of every 6 serialized T1,T2 bits via
//   puncture_mask {1,1,1,0,0,1} -> 3 in / 4 out.
// 5th-grade view: Encode in a loop so the start and end line up, and skip some
//   check-sounds to talk faster.
// -----------------------------------------------------------------------------
Result<std::size_t> encode_tail_biting_punctured_3_4(BitSpan input, MutableBitSpan output) noexcept
{
    if (input.size() < 7U || (input.size() % 3U) != 0U)
    {
        return Status{StatusCode::invalid_argument, "tail-biting input must be >=7 bits and divisible by 3"};
    }

    const auto required = (input.size() * 4U) / 3U;

    if (output.size() < required)
    {
        return Status{StatusCode::buffer_too_small, "punctured FEC output too small"};
    }

    ConvolutionalEncoderK7 encoder;

    for (std::size_t index = 0U; index < 6U; ++index)
    {
        static_cast<void>(encoder.push(input[index]));
    }

    std::size_t unpunctured_index = 0U;
    std::size_t written = 0U;
    auto emit_pair = [&](EncodedPair pair) noexcept
    {
        const std::array<std::uint8_t, 2> bits{pair.t1, pair.t2};

        for (const auto bit : bits)
        {
            if (puncture_mask[unpunctured_index % puncture_mask.size()] != 0U)
            {
                output[written++] = bit;
            }

            ++unpunctured_index;
        }
    };

    for (std::size_t index = 6U; index < input.size(); ++index)
    {
        emit_pair(encoder.push(input[index]));
    }

    for (std::size_t index = 0U; index < 6U; ++index)
    {
        emit_pair(encoder.push(input[index]));
    }

    return written;
}

Status viterbi_decode_rate_half(std::span<const float> soft_bits, MutableBitSpan decoded, MutableBitSpan survivor_scratch, std::uint8_t initial_state, bool require_zero_final_state) noexcept
{
    const auto result = decode_with_end_state(soft_bits, decoded, survivor_scratch, initial_state, require_zero_final_state ? 0 : -1);
    return result ? Status::success() : result.status();
}

// Unreachable-state sentinel. Path metrics are renormalized to zero minimum
// every step, so reachable metrics stay within a few branch magnitudes of
// zero and integer overflow is structurally impossible.
constexpr std::int32_t unreachable_metric = std::numeric_limits<std::int32_t>::max() / 2;

Status StreamingViterbiK7::initialize(std::span<std::uint64_t> survivor_window) noexcept
{
    if (survivor_window.size() < minimum_traceback_depth || survivor_window.size() > 512U)
    {
        return {StatusCode::invalid_argument, "streaming Viterbi traceback window is invalid"};
    }

    survivors_ = survivor_window;

    for (std::size_t register_value = 0U; register_value < branch_pairs_.size(); ++register_value)
    {
        const auto t1 = parity(static_cast<std::uint8_t>(register_value & polynomial_t1));
        const auto t2 = parity(static_cast<std::uint8_t>(register_value & polynomial_t2));
        branch_pairs_[register_value] = static_cast<std::uint8_t>(t1 | (t2 << 1U));
    }

    initialized_ = true;
    restart(0U);
    return Status::success();
}

void StreamingViterbiK7::restart(std::uint8_t initial_state) noexcept
{
    metrics_.fill(unreachable_metric);
    metrics_[initial_state & 0x3FU] = 0;
    newest_step_ = 0U;
    pending_steps_ = 0U;
}

std::uint8_t StreamingViterbiK7::best_state() const noexcept
{
    std::uint8_t best = 0U;

    for (std::uint8_t state = 1U; state < state_count; ++state)
    {
        if (metrics_[state] < metrics_[best])
        {
            best = state;
        }
    }

    return best;
}

// -----------------------------------------------------------------------------
// StreamingViterbiK7::update  (continuous Viterbi, one step)
// -----------------------------------------------------------------------------
// 50K view: Consume one combined metric pair; emit at most one STABLE bit (the one
//   leaving the fixed traceback window). Output is delayed by the window depth.
// Detailed view: One add-compare-select step with INTEGER metrics renormalized to a
//   zero minimum each step (overflow structurally impossible); store the 64
//   predecessor decisions as one uint64 in the ring buffer; once the window is full,
//   trace back `depth` steps and emit the bit falling off the back. Same conventions
//   as the batch decoder, so both are bit-identical on a merged trellis. This is the
//   block-by-block streaming decode mechanism.
// 5th-grade view: Same maze-walk, but only look back a fixed number of rooms; each
//   step finalizes the digit that just fell off the back end.
// -----------------------------------------------------------------------------
std::size_t StreamingViterbiK7::update(std::int32_t soft_t1, std::int32_t soft_t2, MutableBitSpan stable_bits) noexcept
{
    if (!initialized_)
    {
        return 0U;
    }

    next_metrics_.fill(unreachable_metric);
    std::uint64_t decisions{};

    for (std::size_t previous = 0U; previous < state_count; ++previous)
    {
        if (metrics_[previous] == unreachable_metric)
        {
            continue;
        }

        for (std::uint8_t bit = 0U; bit < 2U; ++bit)
        {
            const auto register_value = static_cast<std::size_t>((previous << 1U) | bit);
            const auto next = register_value & 0x3FU;
            const auto pair = branch_pairs_[register_value];
            const auto candidate = metrics_[previous] + ((pair & 1U) == 0U ? -soft_t1 : soft_t1) + ((pair & 2U) == 0U ? -soft_t2 : soft_t2);

            if (candidate < next_metrics_[next])
            {
                next_metrics_[next] = candidate;
                const auto predecessor_high_bit = static_cast<std::uint64_t>((previous >> 5U) & 1U);
                decisions = (decisions & ~(std::uint64_t{1U} << next)) | (predecessor_high_bit << next);
            }
        }
    }

    metrics_ = next_metrics_;
    const auto base = metrics_[best_state()];

    for (auto& metric : metrics_)
    {
        if (metric != unreachable_metric)
        {
            metric -= base;
        }
    }

    const auto depth = survivors_.size();
    survivors_[newest_step_] = decisions;
    newest_step_ = (newest_step_ + 1U) % depth;

    if (pending_steps_ < depth)
    {
        ++pending_steps_;
    }

    if (pending_steps_ < depth || stable_bits.empty())
    {
        return 0U;
    }

    auto state = best_state();
    std::uint8_t oldest_bit = static_cast<std::uint8_t>(state & 1U);

    for (std::size_t back = 0U; back + 1U < depth; ++back)
    {
        const auto slot = (newest_step_ + depth - 1U - back) % depth;
        const auto predecessor_high_bit = static_cast<std::uint8_t>((survivors_[slot] >> state) & 1U);
        state = static_cast<std::uint8_t>((state >> 1U) | (predecessor_high_bit << 5U));
        oldest_bit = static_cast<std::uint8_t>(state & 1U);
    }

    stable_bits[0] = oldest_bit;
    pending_steps_ = depth - 1U;
    return 1U;
}

std::size_t StreamingViterbiK7::flush(MutableBitSpan stable_bits) noexcept
{
    if (!initialized_ || pending_steps_ == 0U || stable_bits.size() < pending_steps_)
    {
        return 0U;
    }

    const auto depth = survivors_.size();
    const auto emitted = pending_steps_;
    auto state = best_state();

    for (std::size_t back = 0U; back < emitted; ++back)
    {
        stable_bits[emitted - 1U - back] = static_cast<std::uint8_t>(state & 1U);
        const auto slot = (newest_step_ + depth - 1U - back) % depth;
        const auto predecessor_high_bit = static_cast<std::uint8_t>((survivors_[slot] >> state) & 1U);
        state = static_cast<std::uint8_t>((state >> 1U) | (predecessor_high_bit << 5U));
    }

    pending_steps_ = 0U;
    return emitted;
}

// -----------------------------------------------------------------------------
// viterbi_decode_tail_biting_3_4
// -----------------------------------------------------------------------------
// 50K view: Decode the punctured, tail-biting 3/4 high-rate code.
// Detailed view: DEPUNCTURE first - reinsert a neutral 0.0 LLR wherever the mask
//   dropped a bit, restoring the full rate-1/2 stream. Then, because the trellis is
//   tail-biting (start state = end state, unknown), try all 64 start states each
//   constrained to end in the SAME state, keep the min-metric path, and restore the
//   original input order (the trellis emits bits 6..N-1 then 0..5).
// 5th-grade view: Fill blanks for the skipped sounds, try every possible loop-start,
//   keep the loop that fits best.
// -----------------------------------------------------------------------------
Status viterbi_decode_tail_biting_3_4(std::span<const float> punctured_soft_bits, MutableBitSpan decoded, std::span<float> depunctured_scratch, MutableBitSpan survivor_scratch) noexcept
{
    if (decoded.size() < 7U || (decoded.size() % 3U) != 0U || punctured_soft_bits.size() != (decoded.size() * 4U) / 3U || depunctured_scratch.size() < decoded.size() * 2U ||
            survivor_scratch.size() < decoded.size() * state_count + decoded.size())
    {
        return {StatusCode::buffer_too_small, "tail-biting decoder buffers do not match block"};
    }

    std::size_t source = 0U;

    for (std::size_t index = 0U; index < decoded.size() * 2U; ++index)
    {
        depunctured_scratch[index] = puncture_mask[index % puncture_mask.size()] != 0U ? punctured_soft_bits[source++] : 0.0F;
    }

    auto best_bits = survivor_scratch.subspan(decoded.size() * state_count, decoded.size());
    float best_metric = std::numeric_limits<float>::infinity();
    bool found = false;

    for (std::uint8_t state = 0U; state < state_count; ++state)
    {
        const auto result = decode_with_end_state(depunctured_scratch.first(decoded.size() * 2U), decoded, survivor_scratch.first(decoded.size() * state_count), state, state);

        if (result && result.value() < best_metric)
        {
            best_metric = result.value();
            found = true;

            for (std::size_t index = 0U; index < decoded.size(); ++index)
            {
                best_bits[index] = decoded[index];
            }
        }
    }

    if (!found)
    {
        return {StatusCode::invalid_argument, "no tail-biting path found"};
    }

    // The trellis emits bits 6..N-1 followed by saved bits 0..5. Restore the
    // original input order expected by the caller.
    for (std::size_t index = 0U; index < 6U; ++index)
    {
        decoded[index] = best_bits[decoded.size() - 6U + index];
    }

    for (std::size_t index = 6U; index < decoded.size(); ++index)
    {
        decoded[index] = best_bits[index - 6U];
    }

    return Status::success();
}

} // namespace m110
