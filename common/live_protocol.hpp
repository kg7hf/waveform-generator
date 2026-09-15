// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Paul R. Decker

#pragma once

// Engineering control protocol, deliberately separate from qualified WFG/1 ARM.
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace waveform_generator::live_protocol
{
inline constexpr char version[] = "WFG-LIVE/1";
inline constexpr std::size_t wire_capacity = 192;
inline constexpr std::uint32_t cw_oscillator_capacity = 4U;
enum class Kind { info, status, counters, load, play, stop, media_host, media_local, generate_file,
                  seed, reference, cw_on, cw_frequency, cw_ci, static_on, static_rate,
                  static_peak, fade, sweep_frequency, sweep_ci, sweep_fade
                };
struct Request
{
    std::uint32_t seq{};
    Kind kind{};
    char name[13] {};
    char run_id[33] {};
    char input_name[13] {};
    char encoder[16] {};
    char profile[32] {};
    std::uint32_t rate{};
    std::uint32_t interleave{}; // 0 short, 1 long, 2 zero
    std::uint64_t integer{};
    bool scheduled{};
    std::uint64_t frame{};
    double value{};
    double end_value{};
    std::uint32_t steps{};
    std::uint32_t interval_ms{};
    std::uint32_t duration_ms{};
    std::uint32_t oscillator{}; // CW slot 0..3; legacy commands address slot 0.
};

// Positive canonical connection sequence is consumed even for a bad command.
// text excludes LF and may end in CR. No embedded controls or extra spaces.
bool parse(std::string_view text, std::uint32_t& last_sequence, Request& request) noexcept;
bool filename(std::string_view text, std::string_view extension) noexcept;
bool uint64(std::string_view text, std::uint64_t& value) noexcept;
} // namespace waveform_generator::live_protocol
