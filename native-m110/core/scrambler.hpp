#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

// =============================================================================
// scrambler.hpp - body whitening, sync sequence, high-rate scrambler, Gray map
// =============================================================================
// BodyDataRandomizer (whitening LFSR), SyncRandomizer (fixed preamble/probe
// sequence), HighRateScrambler (110C PSK/QAM), and modified_gray_decode (the
// constellation bit->tribit mapping shared with the demapper).
// Teaching walkthrough: core/body-waveform-and-scrambler-explainer.md
// =============================================================================

namespace m110
{

class BodyDataRandomizer
{
public:
    BodyDataRandomizer() noexcept
    {
        reset();
    }

    void reset() noexcept;
    [[nodiscard]] std::uint8_t next_tribit() noexcept;
    [[nodiscard]] std::uint16_t state() const noexcept;

    [[nodiscard]] std::uint16_t symbols_generated() const noexcept
    {
        return symbols_generated_;
    }

private:
    void clock() noexcept;

    std::uint16_t state_{};
    std::uint16_t symbols_generated_{};
};

class SyncRandomizer
{
public:
    [[nodiscard]] std::uint8_t next_tribit() noexcept;

    void reset() noexcept
    {
        index_ = 0U;
    }

private:
    std::size_t index_{};
};

class HighRateScrambler
{
public:
    void reset() noexcept
    {
        state_ = 1U;
    }

    [[nodiscard]] std::uint8_t scramble_psk(std::uint8_t symbol, std::uint8_t bits) noexcept;
    [[nodiscard]] std::uint8_t scramble_qam(std::uint8_t symbol, std::uint8_t bits) noexcept;
    [[nodiscard]] std::uint8_t descramble_psk(std::uint8_t symbol, std::uint8_t bits) noexcept;
    [[nodiscard]] std::uint8_t descramble_qam(std::uint8_t symbol, std::uint8_t bits) noexcept;

    [[nodiscard]] std::uint16_t state() const noexcept
    {
        return state_;
    }

private:
    void advance(std::uint8_t clocks) noexcept;
    std::uint16_t state_{1U};
};

[[nodiscard]] constexpr std::uint8_t modified_gray_decode(std::uint8_t value, std::uint8_t width) noexcept
{
    // MIL-STD-188-110B body-waveform modified-Gray mapping. This is not the
    // Appendix C tribit-to-8PSK transcoding table.
    if (width == 1U)
    {
        return static_cast<std::uint8_t>((value & 1U) << 2U);
    }

    if (width == 2U)
    {
        constexpr std::array<std::uint8_t, 4> map{0U, 1U, 3U, 2U};
        return map[value & 3U];
    }

    constexpr std::array<std::uint8_t, 8> map{0U, 1U, 3U, 2U, 7U, 6U, 4U, 5U};
    return map[value & 7U];
}

} // namespace m110
