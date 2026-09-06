/*
================================================================================
scrambler.cpp - the three scramblers + the modified-Gray constellation map
================================================================================
Three DIFFERENT mechanisms for three purposes:
  BodyDataRandomizer  WHITENING: a 12-bit LFSR (seed 0xBAD, 110B Fig 6, 8 clocks/
                      tribit, reset @160 sym). Its tribit is ADDED mod 8 to every
                      transmitted body symbol (subtracted on receive) so the spectrum
                      stays even regardless of the data.
  SyncRandomizer      PREAMBLE/PROBES: a fixed 32-entry sequence cycled by index; the
                      sync scramble on the preamble prefix and probe symbols.
  HighRateScrambler   110C: an x^9+x^4+1 LFSR; scramble/descramble PSK (add mod 8) and
                      QAM (XOR mask).
  modified_gray_decode  the constellation bit->tribit map shared by the demapper and
                      the transmit mapper (adjacent points differ in one bit).
Teaching walkthrough: core/body-waveform-and-scrambler-explainer.md
================================================================================
*/

#include "core/scrambler.hpp"

#include <array>
#include <cstddef>
#include <cstdint>

namespace m110
{
constexpr std::array<std::uint8_t, 32> sync_sequence{7U, 4U, 3U, 0U, 5U, 1U, 5U, 0U, 2U, 2U, 1U, 1U, 5U, 7U, 4U, 3U, 5U, 0U, 2U, 6U, 2U, 1U, 6U, 2U, 0U, 0U, 5U, 0U, 5U, 2U, 6U, 6U};

void BodyDataRandomizer::reset() noexcept
{
    // MIL-STD-188-110B Figure 6, printed page 46. Bit 11 is the leftmost
    // stage and bits 2..0 are the three output stages. The independent
    // DataScramblerFixed comparison recorded in docs/reviews confirms all
    // 160 output symbols for this convention.
    state_ = 0xBADU;
    symbols_generated_ = 0U;
}

// -----------------------------------------------------------------------------
// BodyDataRandomizer::clock / next_tribit  (the whitening LFSR)
// -----------------------------------------------------------------------------
// 50K view: Generate the whitening tribit added to every transmitted body symbol.
// Detailed view: A 12-bit LFSR (seed 0xBAD, 110B Figure 6); feedback from bit 11
//   XORs taps {6,4,1}. next_tribit clocks it 8 times and returns the low 3 bits,
//   resetting after 160 symbols. The output tribit is added mod 8 on transmit and
//   subtracted (via conj derotation) on receive.
// 5th-grade view: A shared pseudo-random dial that both sides turn in lock-step to
//   scramble and unscramble the voice.
// -----------------------------------------------------------------------------
void BodyDataRandomizer::clock() noexcept
{
    constexpr std::uint16_t register_mask = 0x0FFFU;
    constexpr std::uint16_t feedback_taps = (1U << 6U) | (1U << 4U) | (1U << 1U);
    const auto feedback = static_cast<std::uint16_t>((state_ >> 11U) & 1U);
    state_ = static_cast<std::uint16_t>((state_ << 1U) & register_mask);

    if (feedback != 0U)
    {
        state_ ^= feedback_taps;
        state_ |= 1U;
    }
}

std::uint8_t BodyDataRandomizer::next_tribit() noexcept
{
    if (symbols_generated_ == 160U)
    {
        reset();
    }

    for (std::uint8_t count = 0U; count < 8U; ++count)
    {
        clock();
    }

    ++symbols_generated_;
    return static_cast<std::uint8_t>(state_ & 0x07U);
}

std::uint16_t BodyDataRandomizer::state() const noexcept
{
    return state_;
}

std::uint8_t SyncRandomizer::next_tribit() noexcept
{
    const auto value = sync_sequence[index_];
    index_ = (index_ + 1U) % sync_sequence.size();
    return value;
}

void HighRateScrambler::advance(std::uint8_t clocks) noexcept
{
    for (std::uint8_t count = 0U; count < clocks; ++count)
    {
        // x^9 + x^4 + 1, figure C-5. State bit 0 is the rightmost x^1
        // stage and bit 8 is the leftmost x^9 stage.
        const auto feedback = static_cast<std::uint16_t>(((state_ >> 0U) ^ (state_ >> 4U)) & 1U);
        state_ = static_cast<std::uint16_t>((state_ >> 1U) | (feedback << 8U));
    }
}

std::uint8_t HighRateScrambler::scramble_psk(std::uint8_t symbol, std::uint8_t bits) noexcept
{
    // PSK scrambling operates on the 8-PSK channel symbol regardless of the
    // underlying data rate, so it always consumes three sequence clocks per
    // symbol (advance(3U) below). Only the QPSK-subset (bits==2) and 8-PSK
    // (bits==3) mappings are defined; reject anything else rather than apply
    // 8-PSK semantics to an unsupported mode.
    if (bits < 2U || bits > 3U)
    {
        return symbol;
    }

    const auto key = static_cast<std::uint8_t>(state_ & 0x07U);
    const auto result = static_cast<std::uint8_t>((symbol + key) & 0x07U);
    advance(3U);
    return result;
}

std::uint8_t HighRateScrambler::scramble_qam(std::uint8_t symbol, std::uint8_t bits) noexcept
{
    if (bits < 4U || bits > 6U)
    {
        return symbol;
    }

    const auto mask = static_cast<std::uint8_t>((1U << bits) - 1U);
    const auto result = static_cast<std::uint8_t>(symbol ^ (state_ & mask));
    advance(bits);
    return result;
}

std::uint8_t HighRateScrambler::descramble_psk(std::uint8_t symbol, std::uint8_t bits) noexcept
{
    // PSK scrambling operates on the 8-PSK channel symbol regardless of the
    // underlying data rate, so it always consumes three sequence clocks per
    // symbol (advance(3U) below). Only the QPSK-subset (bits==2) and 8-PSK
    // (bits==3) mappings are defined; reject anything else rather than apply
    // 8-PSK semantics to an unsupported mode.
    if (bits < 2U || bits > 3U)
    {
        return symbol;
    }

    const auto key = static_cast<std::uint8_t>(state_ & 0x07U);
    const auto result = static_cast<std::uint8_t>((symbol + 8U - key) & 0x07U);
    advance(3U);
    return result;
}

std::uint8_t HighRateScrambler::descramble_qam(std::uint8_t symbol, std::uint8_t bits) noexcept
{
    if (bits < 4U || bits > 6U)
    {
        return symbol;
    }

    const auto mask = static_cast<std::uint8_t>((1U << bits) - 1U);
    const auto result = static_cast<std::uint8_t>(symbol ^ (state_ & mask));
    advance(bits);
    return result;
}

} // namespace m110
