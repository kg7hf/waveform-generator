// SPDX-License-Identifier: GPL-3.0-only
// SPDX-FileCopyrightText: 2026 Paul R. Decker

#pragma once

#include "common/card_listing.hpp"
#include "common/status.hpp"

#include <cstdint>

namespace waveform_generator::card_listing
{

inline constexpr m110::Status open_failed{.code = m110::StatusCode::io_error, .message = "FILES_OPEN"};
inline constexpr m110::Status read_failed{.code = m110::StatusCode::io_error, .message = "FILES_READ"};
inline constexpr m110::Status close_failed{.code = m110::StatusCode::io_error, .message = "FILES_CLOSE"};
inline constexpr m110::Status invalid_name{.code = m110::StatusCode::invalid_argument, .message = "FILES_NAME"};
inline constexpr m110::Status invalid_offset{.code = m110::StatusCode::invalid_argument, .message = "FILES_RANGE"};

// Reads one stable FatFs-order page from an already-mounted 2:/WG directory.
// Each name is the 8.3 alias when FatFs supplies one, otherwise its bounded
// CP437 fname (exFAT has no SFN alias); entries are never truncated. On failure,
// page is cleared and must not be published to a caller.
[[nodiscard]] m110::Status read_page(std::uint64_t offset, Page& page) noexcept;

} // namespace waveform_generator::card_listing
