// SPDX-License-Identifier: GPL-3.0-only
// SPDX-FileCopyrightText: 2026 Paul R. Decker

#pragma once

#include <array>
#include <cstdint>

namespace waveform_generator::card_listing
{

inline constexpr std::uint32_t page_capacity = 24U;
inline constexpr std::uint32_t name_capacity = 37U;

struct Entry
{
    char name[name_capacity]{};
    std::uint64_t size_bytes{};
};

struct Page
{
    std::uint64_t offset{};
    std::uint64_t total{};
    std::uint32_t count{};
    std::array<Entry, page_capacity> entries{};
};

} // namespace waveform_generator::card_listing
