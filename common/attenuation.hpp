// SPDX-License-Identifier: GPL-3.0-only
// SPDX-FileCopyrightText: 2026 Paul R. Decker

// A PLAYLIST item's attenuation (att=): 0 to 40.0 dB below full level in
// steps of 0.1 dB, applied to the item's output after the engine, in exact
// integer arithmetic
// Guide: docs/modules/rt1170-player-and-audio.md (PLAYLIST sequencer),
// docs/live-control-protocol.md (PLAYLIST)
// Ownership: plain values; the gain table is constant (flash on the board).
// Contract: constexpr, allocation-free, no I/O. n tenths of a dB is the gain
// 10^(-n/200) as a Q1.31 fraction from attenuation_gains.hpp, which
// host/attenuation.py writes from that closed form (each entry rounded half
// to even). A PCM24 sample s becomes round_half_even(s * gain / 2^31): zero
// stays zero, 0 dB passes every sample through unchanged, and the result
// never leaves the PCM24 range. The gain is memoryless per sample, so its
// order against the CUT stage (which drops frames or inserts zeros) does not
// change the stream.

#pragma once

#include "common/attenuation_gains.hpp"
#include "common/pcm24.hpp"
#include "common/status.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <string_view>
#include <type_traits>
#include <utility>

namespace waveform_generator::attenuation
{

inline constexpr std::uint32_t max_tenths = 400U;

// The refusal of an att= value: positive, more than one decimal, past
// -40.0 dB, or another spelling.
inline constexpr m110::Status bad_attenuation{.code = m110::StatusCode::invalid_argument, .message = "PLAYLIST_ATT"};

// An attenuation in tenths of a decibel below full level: 0 (none) to
// max_tenths (-40.0 dB). The default is 0 dB; of() alone makes another, and
// refuses one past max_tenths, so every Tenths has a gain in the table
// (Gain::of() is total) and the PLAYLIST? block's att_tenths_db at most
// three digits.
class Tenths
{
public:
    constexpr Tenths() noexcept = default;

    // `tenths` tenths of a dB, else bad_attenuation past max_tenths.
    [[nodiscard]] static constexpr m110::Result<Tenths> of(std::uint32_t tenths) noexcept
    {
        if (tenths > max_tenths)
        {
            return bad_attenuation;
        }

        return Tenths{tenths};
    }

    [[nodiscard]] constexpr std::uint32_t value() const noexcept { return value_; }

    constexpr auto operator<=>(const Tenths&) const noexcept = default;

private:
    explicit constexpr Tenths(std::uint32_t tenths) noexcept : value_{tenths} {}

    std::uint32_t value_{};
};

static_assert(!std::is_constructible_v<Tenths, std::uint32_t> && !std::is_aggregate_v<Tenths>, "only of() makes an attenuation from a number");

inline constexpr Tenths none{};
inline constexpr Tenths most = Tenths::of(max_tenths).value();
static_assert(!Tenths::of(max_tenths + 1U).has_value() && most.value() == max_tenths, "an attenuation is 0 to -40.0 dB");
inline constexpr std::uint32_t q_bits = 31U;
inline constexpr std::uint32_t unity_q31 = std::uint32_t{1} << q_bits;
inline constexpr std::uint32_t tenths_per_db = 10U;
inline constexpr std::uint32_t decimal_radix = 10U;

static_assert(gains_q31.size() == std::size_t{max_tenths} + 1U, "one gain per tenth of a dB, 0 to -40.0 dB: one for every Tenths");
static_assert(gains_q31.front() == unity_q31, "0 dB is the gain 1 exactly");

// A gain of the table, made only from an attenuation: the table has an
// entry for each one a Tenths can hold.
class Gain
{
public:
    [[nodiscard]] static constexpr Gain of(Tenths attenuation) noexcept { return Gain{std::span{gains_q31}[attenuation.value()]}; }

    // The Q1.31 fraction: unity_q31 is 1.
    [[nodiscard]] constexpr std::uint32_t q31() const noexcept { return q31_; }

private:
    explicit constexpr Gain(std::uint32_t q31) noexcept : q31_{q31} {}

    std::uint32_t q31_;
};

// round_half_even(product / 2^31). The right shift of a negative product is
// floor division (C++20), and the remainder below 2^31 decides the rounding:
// up past half, and at half only from an odd quotient.
[[nodiscard]] constexpr std::int64_t round_q31(std::int64_t product) noexcept
{
    constexpr auto unity = std::int64_t{unity_q31};
    constexpr std::int64_t half = unity / 2;
    const std::int64_t quotient = product >> q_bits;
    const std::int64_t remainder = product - quotient * unity;
    const bool odd = quotient % 2 != 0;
    return quotient + (remainder > half || (remainder == half && odd) ? 1 : 0);
}

// round_half_even(sample * gain / 2^31): the product needs 55 bits.
[[nodiscard]] constexpr audio::Pcm24Sample apply(Gain gain, audio::Pcm24Sample sample) noexcept
{
    const std::int64_t rounded = round_q31(std::int64_t{sample} * std::int64_t{gain.q31()});
    // |sample * gain| <= |sample| * 2^31, so |rounded| <= |sample|: a PCM24
    // sample, which the narrowing keeps exactly.
    return static_cast<audio::Pcm24Sample>(rounded);
}

// A decimal digit's value, or none.
[[nodiscard]] constexpr std::optional<std::uint32_t> digit(char character) noexcept
{
    constexpr std::string_view digits{"0123456789"};
    std::uint32_t value = 0U;

    for (const char candidate : digits)
    {
        if (candidate == character)
        {
            return value;
        }

        ++value;
    }

    return std::nullopt;
}

// The whole decibels of an att= value: "0", or 1 to 2 digits without a
// leading zero.
[[nodiscard]] constexpr std::optional<std::uint32_t> whole_decibels(std::string_view text) noexcept
{
    constexpr std::size_t most_digits = 2U;

    if (text.empty() || text.size() > most_digits || (text.size() > 1U && text.front() == '0'))
    {
        return std::nullopt;
    }

    std::uint32_t value = 0U;

    for (const char character : text)
    {
        const auto place = digit(character);

        if (!place.has_value())
        {
            return std::nullopt;
        }

        value = value * decimal_radix + *place;
    }

    return value;
}

// att=<dB>: 0 (or 0.0, -0), or a negative level down to -40.0 with at most
// one decimal ("-20", "-0.5", "-40.0"). Never a positive one: the player
// only lowers the level. Else bad_attenuation.
[[nodiscard]] constexpr m110::Result<Tenths> parse(std::string_view text) noexcept
{
    const bool negative = text.starts_with('-');
    const auto magnitude = negative ? text.substr(1U) : text;
    const auto point = magnitude.find('.');
    const auto whole = whole_decibels(magnitude.substr(0U, point));
    const auto decimals = point == std::string_view::npos ? std::string_view{"0"} : magnitude.substr(point + 1U);
    const auto tenth = decimals.size() == 1U ? digit(decimals.front()) : std::nullopt;

    if (!whole.has_value() || !tenth.has_value())
    {
        return bad_attenuation;
    }

    const std::uint32_t tenths = *whole * tenths_per_db + *tenth;

    if (tenths != 0U && !negative)
    {
        return bad_attenuation;
    }

    return Tenths::of(tenths);
}

// A worked example of the grammar: a text and the tenths it spells, or none
// for a refusal.
struct Spelling
{
    std::string_view text;
    std::optional<std::uint32_t> tenths;
};

// The grammar: 0 three ways, levels down to -40.0 with one decimal at most;
// refused, a positive level, a second decimal, a level past -40.0, a leading
// zero, a bare point or sign.
[[nodiscard]] consteval bool spellings_hold() noexcept
{
    constexpr auto examples = std::to_array<Spelling>({
        {.text = "0", .tenths = 0U},
        {.text = "0.0", .tenths = 0U},
        {.text = "-0", .tenths = 0U},
        {.text = "-20", .tenths = 200U},
        {.text = "-0.5", .tenths = 5U},
        {.text = "-40.0", .tenths = max_tenths},
        {.text = "-40.1", .tenths = std::nullopt},
        {.text = "-41", .tenths = std::nullopt},
        {.text = "3", .tenths = std::nullopt},
        {.text = "0.5", .tenths = std::nullopt},
        {.text = "-5.25", .tenths = std::nullopt},
        {.text = "-05", .tenths = std::nullopt},
        {.text = "-.5", .tenths = std::nullopt},
        {.text = "-5.", .tenths = std::nullopt},
        {.text = "+0", .tenths = std::nullopt},
        {.text = "", .tenths = std::nullopt},
        {.text = "-", .tenths = std::nullopt},
        {.text = "--1", .tenths = std::nullopt},
        {.text = "-100", .tenths = std::nullopt},
    });
    return std::ranges::all_of(examples,
                               [](const Spelling& example)
                               {
                                   const auto parsed = parse(example.text);
                                   return parsed.has_value() ? example.tenths == parsed.value().value() : !example.tenths.has_value();
                               });
}

// A worked example of the arithmetic: a sample at an attenuation and what
// it becomes.
struct Product
{
    Tenths attenuation;
    audio::Pcm24Sample sample;
    audio::Pcm24Sample result;
};

// 0 dB leaves the extremes; zero stays zero; -20 dB is a tenth of full
// scale, symmetric about zero; -40 dB is a hundredth.
[[nodiscard]] consteval bool products_hold() noexcept
{
    constexpr Tenths twenty_db = Tenths::of(200U).value();
    constexpr auto examples = std::to_array<Product>({
        {.attenuation = none, .sample = audio::pcm24_min, .result = audio::pcm24_min},
        {.attenuation = none, .sample = audio::pcm24_max, .result = audio::pcm24_max},
        {.attenuation = twenty_db, .sample = 0, .result = 0},
        {.attenuation = most, .sample = 0, .result = 0},
        {.attenuation = twenty_db, .sample = 10000, .result = 1000},
        {.attenuation = twenty_db, .sample = -10000, .result = -1000},
        {.attenuation = twenty_db, .sample = 5, .result = 1},
        {.attenuation = twenty_db, .sample = -5, .result = -1},
        {.attenuation = most, .sample = audio::pcm24_min, .result = -83886},
    });
    return std::ranges::all_of(examples, [](const Product& example) { return apply(Gain::of(example.attenuation), example.sample) == example.result; });
}

// Ties, which no gain of the table but 0 dB meets within PCM24: half of 1,
// 3, -1 and -3 rounds to the even 0, 2, 0 and -2; just past half rounds
// away from zero.
[[nodiscard]] consteval bool ties_hold() noexcept
{
    constexpr std::int64_t half = std::int64_t{unity_q31} / 2;
    constexpr std::int64_t three = 3;
    constexpr auto examples = std::to_array<std::pair<std::int64_t, std::int64_t>>({
        {half, 0},
        {three * half, 2},
        {-half, 0},
        {-three * half, -2},
        {half + 1, 1},
        {-half - 1, -1},
    });
    return std::ranges::all_of(examples, [](const std::pair<std::int64_t, std::int64_t>& example) { return round_q31(example.first) == example.second; });
}

// The table falls with every tenth, and -20 dB is 2^31 / 10 (214748364.8)
// rounded.
[[nodiscard]] consteval bool table_falls() noexcept
{
    constexpr Tenths twenty_db = Tenths::of(200U).value();
    constexpr std::uint32_t tenth_q31 = 214748365U;
    const std::span gains{gains_q31};
    bool falling = true;

    for (std::size_t index = 1U; index < gains.size(); ++index)
    {
        falling = falling && gains[index] < gains[index - 1U];
    }

    return falling && Gain::of(twenty_db).q31() == tenth_q31;
}

static_assert(spellings_hold(), "att= spells 0 to -40.0 dB in tenths and never a positive level");
static_assert(products_hold() && ties_hold(), "the gains are exact Q1.31 fractions, the products rounded half to even; 0 dB and zero pass unchanged");
static_assert(table_falls(), "the table falls from 1 with every tenth of a dB");
static_assert(std::numeric_limits<std::int64_t>::max() / unity_q31 > std::int64_t{audio::pcm24_max}, "a sample times a gain fits 64 bits");

} // namespace waveform_generator::attenuation
