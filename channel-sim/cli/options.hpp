// SPDX-License-Identifier: GPL-3.0-only
// SPDX-FileCopyrightText: 2026 Paul R. Decker

// pathsim_chain's command line (guide: docs/modules/pathsim.md).
//
// The channel options are the patched pathsim CLI's, by the same names: a preset
// flag (--ccir-poor, --direct, ...), --snr, --spread/--offset, --delay2/--spread2/
// --offset2, --delay3/--spread3/--offset3, --seed, --truth-out, the input and output
// as positionals or -i/--input_file and -o/--output_file, and --help. Values follow
// a space or '='. The chain's own options are --chain 8k|48k|auto and
// --resample-gain-db (48 kHz chain). Unlike pathsim (cxxopts with unrecognised
// options allowed), an unknown, repeated or malformed option is an error.

#pragma once

#include "common/status.hpp"
#include "core/channel_spec.hpp"
#include "core/pathsim_driver.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

namespace channel_sim::cli
{

enum class ChainMode : std::uint8_t
{
    automatic,  // from the input rate: 8000 Hz -> pathsim, 48000 Hz -> lane chain
    pathsim_8k, // 8 kHz in, pathsim, 8 kHz out (pathsim.exe's job)
    lane_48k    // 48 kHz in, the lane chain, 48 kHz out (run_pathsim.py's job)
};

struct CliOptions
{
    const char* input{}; // file names: NUL-terminated, inside the arguments
    const char* output{};
    const char* truth{};
    std::string_view preset{};
    std::optional<double> snr{};
    std::optional<std::uint32_t> seed{};
    std::array<std::optional<double>, max_paths> delay{}; // [0] unused: path 1 has no delay option
    std::array<std::optional<double>, max_paths> spread{};
    std::array<std::optional<double>, max_paths> offset{};
    std::optional<ChainMode> mode{}; // none: automatic
    std::optional<double> resample_gain_db{};
    bool help{};
};

class CliParser
{
public:
    // arguments excludes the program name. Names and views point into the arguments.
    [[nodiscard]] m110::Status parse(std::span<const char* const> arguments) noexcept;
    [[nodiscard]] const CliOptions& options() const noexcept;
    // The argument that failed to parse, for the error message.
    [[nodiscard]] std::string_view failed_argument() const noexcept;

private:
    [[nodiscard]] m110::Status parse_long(std::span<const char* const> arguments, std::size_t& index) noexcept;
    // value is NUL-terminated (an argument, or the tail of one after '=').
    [[nodiscard]] m110::Status apply(std::string_view name, const char* value) noexcept;
    [[nodiscard]] m110::Status positional(const char* value) noexcept;

    CliOptions options_{};
    std::string_view failed_{};
};

// The job in pathsim main.cpp's order: preset, then --snr, then the path options.
[[nodiscard]] m110::Result<PathsimJob> build_job(const CliOptions& options) noexcept;

} // namespace channel_sim::cli
