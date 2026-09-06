/*
================================================================================
interleaver.cpp - MIL-STD-188-110B matrix interleaver + high-rate interleaver
================================================================================

WHAT THIS FILE IS
-----------------
Reorders coded bits so a channel FADE (which corrupts a contiguous run of received
symbols) becomes SCATTERED coded-bit errors after unscrambling - which convolutional
codes tolerate, unlike long bursts. Transmit INTERLEAVES; receive DEINTERLEAVES; the
turbo loop uses the soft variants both directions. The soft variants move the
demapper's LLRs to the SISO decoder (encoder order) and the decoder's a-priori back
to the demapper (channel order); they only reorder the LLRs and never rescale them,
so the demapper's effective-variance calibration and the SISO log_map lever (turbo
reference: core/turbo-equalizer-and-burst-decoder-explainer.md) are unaffected here.

  INPUT  : a bit or soft-LLR vector + an InterleaverSpec (matrix dims / increments).
  OUTPUT : the permuted vector (interleave) or its inverse (deinterleave).
  DEPENDS: waveform.hpp (InterleaverSpec). Caller owns the scratch matrix; no heap.

THE BODY MATRIX INTERLEAVER
---------------------------
A rows x columns matrix. LOAD (transmit scatter) writes down columns stepping rows by
load_row_increment; FETCH (transmit gather) reads across rows offsetting columns by
fetch_column_decrement. The staggered read is what spreads a burst.
  interleave   = scatter by load,  gather by fetch
  deinterleave = scatter by fetch, gather by load   (EXACT inverse; soft path too)

THE HIGH-RATE INTERLEAVER
-------------------------
A multiplicative permutation addr = index*increment mod size, valid iff
gcd(increment, size) = 1 (a bijection around the ring).

WORKED EXAMPLE / DEEP DIVE
--------------------------
    core/demapper-and-deinterleaver-explainer.md
================================================================================
*/

#include "core/interleaver.hpp"

#include <cstddef>
#include <cstdint>
#include <numeric>
#include <span>

namespace m110
{

// -----------------------------------------------------------------------------
// body_load_address / body_fetch_address  (the MIL-STD-188-110B matrix permutation)
// -----------------------------------------------------------------------------
// 50K view: The two halves of the body interleaver over a rows x columns matrix.
// Detailed view: LOAD (transmit scatter) writes down columns stepping the row by
//   load_row_increment: row = (i%rows * load_row_increment)%rows, col = i/rows.
//   FETCH (transmit gather) reads across rows offsetting the column by
//   fetch_column_decrement: row = i%rows, col = (i/rows - row*fetch_column_decrement)
//   mod columns. address = row*columns + col. interleave = scatter load / gather
//   fetch; deinterleave swaps them (exact inverse). bypass returns the index.
// 5th-grade view: Fill the grid down the columns in one hop pattern, read it back
//   across the rows in a different hop pattern; the mismatch shuffles a burst apart.
// -----------------------------------------------------------------------------
Result<std::size_t> body_load_address(std::size_t input_index, const InterleaverSpec& spec) noexcept
{
    if (spec.bypass)
    {
        return input_index;
    }

    if (spec.rows == 0U || spec.columns == 0U || input_index >= spec.size_bits)
    {
        return Status{StatusCode::invalid_argument, "invalid body load address request"};
    }

    const auto within_column = input_index % spec.rows;
    const auto column = input_index / spec.rows;
    const auto row = (within_column * spec.load_row_increment) % spec.rows;
    return static_cast<std::size_t>(row) * spec.columns + column;
}

// body_fetch_address: the FETCH half of the pair above (transmit gather / receive
// scatter). See the banner on body_load_address for the full matrix permutation.
Result<std::size_t> body_fetch_address(std::size_t output_index, const InterleaverSpec& spec) noexcept
{
    if (spec.bypass)
    {
        return output_index;
    }

    if (spec.rows == 0U || spec.columns == 0U || output_index >= spec.size_bits)
    {
        return Status{StatusCode::invalid_argument, "invalid body fetch address request"};
    }

    const auto row = output_index % spec.rows;
    const auto row_cycle = output_index / spec.rows;
    const auto column = (row_cycle + spec.columns - ((row * spec.fetch_column_decrement) % spec.columns)) % spec.columns;
    return static_cast<std::size_t>(row) * spec.columns + column;
}

// -----------------------------------------------------------------------------
// high_rate_address  (multiplicative high-rate interleaver)
// -----------------------------------------------------------------------------
// 50K view: The high-rate waveform's simpler permutation: addr = index*increment mod
//   size, a bijection iff gcd(increment, size) = 1.
// 5th-grade view: Shuffle by skip-counting around a ring; a coprime skip visits every
//   seat exactly once.
// -----------------------------------------------------------------------------
Result<std::size_t> high_rate_address(std::size_t index, const InterleaverSpec& spec) noexcept
{
    if (spec.size_bits == 0U || spec.increment == 0U || index >= spec.size_bits || std::gcd(spec.increment, spec.size_bits) != 1U)
    {
        return Status{StatusCode::invalid_argument, "invalid high-rate interleaver address request"};
    }

    const auto product = static_cast<std::uint64_t>(index) * static_cast<std::uint64_t>(spec.increment);
    return static_cast<std::size_t>(product % spec.size_bits);
}

Status body_interleave(BitSpan input, MutableBitSpan scratch, MutableBitSpan output, const InterleaverSpec& spec) noexcept
{
    if (spec.bypass)
    {
        if (output.size() < input.size())
        {
            return {StatusCode::buffer_too_small, "body output too small"};
        }

        for (std::size_t index = 0U; index < input.size(); ++index)
        {
            output[index] = input[index];
        }

        return Status::success();
    }

    if (input.size() != spec.size_bits || scratch.size() < spec.size_bits || output.size() < spec.size_bits)
    {
        return {StatusCode::invalid_argument, "body interleaver buffers do not match matrix"};
    }

    for (std::size_t index = 0U; index < input.size(); ++index)
    {
        const auto address = body_load_address(index, spec);

        if (!address)
        {
            return address.status();
        }

        scratch[address.value()] = input[index];
    }

    for (std::size_t index = 0U; index < input.size(); ++index)
    {
        const auto address = body_fetch_address(index, spec);

        if (!address)
        {
            return address.status();
        }

        output[index] = scratch[address.value()];
    }

    return Status::success();
}

Status body_deinterleave(BitSpan input, MutableBitSpan scratch, MutableBitSpan output, const InterleaverSpec& spec) noexcept
{
    if (spec.bypass)
    {
        return body_interleave(input, scratch, output, spec);
    }

    if (input.size() != spec.size_bits || scratch.size() < spec.size_bits || output.size() < spec.size_bits)
    {
        return {StatusCode::invalid_argument, "body deinterleaver buffers do not match matrix"};
    }

    for (std::size_t index = 0U; index < input.size(); ++index)
    {
        const auto address = body_fetch_address(index, spec);

        if (!address)
        {
            return address.status();
        }

        scratch[address.value()] = input[index];
    }

    for (std::size_t index = 0U; index < input.size(); ++index)
    {
        const auto address = body_load_address(index, spec);

        if (!address)
        {
            return address.status();
        }

        output[index] = scratch[address.value()];
    }

    return Status::success();
}

Status body_deinterleave_soft(std::span<const float> input, std::span<float> scratch, std::span<float> output, const InterleaverSpec& spec) noexcept
{
    if (spec.bypass)
    {
        if (output.size() < input.size())
        {
            return {StatusCode::buffer_too_small, "soft body output too small"};
        }

        for (std::size_t index = 0U; index < input.size(); ++index)
        {
            output[index] = input[index];
        }

        return Status::success();
    }

    if (input.size() != spec.size_bits || scratch.size() < spec.size_bits || output.size() < spec.size_bits)
    {
        return {StatusCode::invalid_argument, "soft body deinterleaver buffers do not match matrix"};
    }

    for (std::size_t index = 0U; index < input.size(); ++index)
    {
        const auto address = body_fetch_address(index, spec);

        if (!address)
        {
            return address.status();
        }

        scratch[address.value()] = input[index];
    }

    for (std::size_t index = 0U; index < input.size(); ++index)
    {
        const auto address = body_load_address(index, spec);

        if (!address)
        {
            return address.status();
        }

        output[index] = scratch[address.value()];
    }

    return Status::success();
}

Status body_interleave_soft(std::span<const float> input, std::span<float> scratch, std::span<float> output, const InterleaverSpec& spec) noexcept
{
    if (spec.bypass)
    {
        if (output.size() < input.size())
        {
            return {StatusCode::buffer_too_small, "soft body output too small"};
        }

        for (std::size_t index = 0U; index < input.size(); ++index)
        {
            output[index] = input[index];
        }

        return Status::success();
    }

    if (input.size() != spec.size_bits || scratch.size() < spec.size_bits || output.size() < spec.size_bits)
    {
        return {StatusCode::invalid_argument, "soft body interleaver buffers do not match matrix"};
    }

    // The transmit walk: load addresses scatter the input into the matrix,
    // fetch addresses gather it out - exactly what body_deinterleave_soft
    // undoes, so the pair round-trips every value untouched.
    for (std::size_t index = 0U; index < input.size(); ++index)
    {
        const auto address = body_load_address(index, spec);

        if (!address)
        {
            return address.status();
        }

        scratch[address.value()] = input[index];
    }

    for (std::size_t index = 0U; index < input.size(); ++index)
    {
        const auto address = body_fetch_address(index, spec);

        if (!address)
        {
            return address.status();
        }

        output[index] = scratch[address.value()];
    }

    return Status::success();
}

Status high_rate_interleave(BitSpan input, MutableBitSpan scratch, MutableBitSpan output, const InterleaverSpec& spec) noexcept
{
    if (input.size() != spec.size_bits || scratch.size() < spec.size_bits || output.size() < spec.size_bits)
    {
        return {StatusCode::invalid_argument, "high-rate interleaver buffer/spec mismatch"};
    }

    for (std::size_t index = 0U; index < input.size(); ++index)
    {
        const auto address = high_rate_address(index, spec);

        if (!address)
        {
            return address.status();
        }

        scratch[address.value()] = input[index];
    }

    for (std::size_t index = 0U; index < input.size(); ++index)
    {
        output[index] = scratch[index];
    }

    return Status::success();
}

Status high_rate_deinterleave(BitSpan input, MutableBitSpan scratch, MutableBitSpan output, const InterleaverSpec& spec) noexcept
{
    if (input.size() != spec.size_bits || scratch.size() < spec.size_bits || output.size() < spec.size_bits)
    {
        return {StatusCode::invalid_argument, "high-rate deinterleaver buffer/spec mismatch"};
    }

    for (std::size_t index = 0U; index < input.size(); ++index)
    {
        scratch[index] = input[index];
    }

    for (std::size_t index = 0U; index < input.size(); ++index)
    {
        const auto address = high_rate_address(index, spec);

        if (!address)
        {
            return address.status();
        }

        output[index] = scratch[address.value()];
    }

    return Status::success();
}

Status high_rate_deinterleave_soft(std::span<const float> input, std::span<float> scratch, std::span<float> output, const InterleaverSpec& spec) noexcept
{
    if (input.size() != spec.size_bits || scratch.size() < spec.size_bits || output.size() < spec.size_bits)
    {
        return {StatusCode::invalid_argument, "soft high-rate deinterleaver buffer/spec mismatch"};
    }

    for (std::size_t index = 0U; index < input.size(); ++index)
    {
        scratch[index] = input[index];
    }

    for (std::size_t index = 0U; index < input.size(); ++index)
    {
        const auto address = high_rate_address(index, spec);

        if (!address)
        {
            return address.status();
        }

        output[index] = scratch[address.value()];
    }

    return Status::success();
}

} // namespace m110
