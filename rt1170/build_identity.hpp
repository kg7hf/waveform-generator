// SPDX-License-Identifier: GPL-3.0-only
// SPDX-FileCopyrightText: 2026 Paul R. Decker

// Firmware build identity: the Git commit and dirty state INFO and STATUS report.
// Guide: docs/modules/build-and-dependencies.md
// Contract: cmake/wfg_build_info.cmake generates wfg_build_info.hpp in the build
// tree. It defines waveform_generator::build_info::fw_commit, fw_commit_hi,
// fw_commit_lo and fw_dirty with the types and meanings below. Only
// FwDirty::clean means that tracked public sources match fw_commit; it does
// not identify the compiler, build options or separately checked-out plugins.

#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <utility>

namespace waveform_generator
{

// fw_dirty as INFO and STATUS report it, in decimal. The numbers are wire
// values: never renumber or reuse one.
enum class FwDirty : std::uint8_t
{
    clean = 0U,              // tracked files match fw_commit
    modified = 1U,           // git status --porcelain --untracked-files=no lists a change
    unknown = 2U,            // no Git, no checkout of its own, or no commit
    clean_with_plugins = 3U, // clean, plus encoder plugin sources fw_commit does not cover
};

// The values docs/live-control-protocol.md publishes for fw_dirty.
static_assert(std::to_underlying(FwDirty::clean) == 0U && std::to_underlying(FwDirty::modified) == 1U && std::to_underlying(FwDirty::unknown) == 2U &&
                  std::to_underlying(FwDirty::clean_with_plugins) == 3U,
              "the published fw_dirty values");

// fw_commit when the build cannot name a commit.
inline constexpr std::string_view unknown_commit{"unknown"};
// git rev-parse HEAD: a SHA-1 object name in lowercase hex.
inline constexpr std::size_t commit_hex_digits = 40U;
// fw_commit_hi and fw_commit_lo each carry this many leading hex digits, in order.
inline constexpr std::size_t commit_word_hex_digits = 8U;

static_assert(unknown_commit.size() <= commit_hex_digits, "STATUS budgets one commit width for both forms");

// True when text is exactly digits lowercase hex digits.
[[nodiscard]] constexpr bool is_lower_hex(std::string_view text, std::size_t digits) noexcept
{
    return text.size() == digits && text.find_first_not_of("0123456789abcdef") == std::string_view::npos;
}

// True when text is a full commit name as git rev-parse HEAD prints it.
[[nodiscard]] constexpr bool is_commit_id(std::string_view text) noexcept
{
    return is_lower_hex(text, commit_hex_digits);
}

} // namespace waveform_generator
