// SPDX-License-Identifier: GPL-3.0-only
// SPDX-FileCopyrightText: 2026 Paul R. Decker

#pragma once

// A count of the global allocations made while counting is on, for the "no
// allocation on the audio path" tests (Criterion 8). allocation_counter.cpp
// replaces this test binary's global operator new and delete (every replaceable
// form: plain, array, nothrow and aligned); they forward to the C heap and count
// each allocation while a CountedRegion is alive. Catch2 itself must not run
// inside a region (it allocates for its messages), so a test opens the region
// around the calls under test only and checks the count after it closes.

#include <cstdint>

namespace signal_lab_test
{

class CountedRegion
{
public:
    CountedRegion() noexcept;
    ~CountedRegion();
    CountedRegion(const CountedRegion&) = delete;
    CountedRegion& operator=(const CountedRegion&) = delete;
    CountedRegion(CountedRegion&&) = delete;
    CountedRegion& operator=(CountedRegion&&) = delete;

    // The allocations since the region opened.
    [[nodiscard]] std::uint64_t allocations() const noexcept;

private:
    std::uint64_t mStart{};
};

// The allocations counted over the binary's lifetime inside regions.
[[nodiscard]] std::uint64_t counted_allocations() noexcept;

// Lets a pointer escape into another unit, so the compiler cannot elide the
// new-expression that made it ([expr.new]: an unescaped pair may be omitted).
void retain(const void* pointer) noexcept;

} // namespace signal_lab_test
