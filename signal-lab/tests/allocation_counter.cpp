// SPDX-License-Identifier: GPL-3.0-only
// SPDX-FileCopyrightText: 2026 Paul R. Decker

// The counting global allocation functions of the Catch2 test binary
// (allocation_counter.hpp). The replaceable operator new and delete are a
// non-owned boundary ([new.delete], [replacement.functions]): they must take and
// return void* and reach the C heap, so the C heap calls below are the only
// ones in signal-lab, each marked where clang-tidy would otherwise report it.
// The tests are single-threaded (Catch2 runs one case at a time); the counter
// is atomic all the same, so a library thread would be counted, not torn.

#include "allocation_counter.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <new>

#if defined(_WIN32)
#include <malloc.h>
#endif

namespace
{

// The counter's state: constant-initialised (no guard, so ready before any static
// constructor allocates) and reached through one accessor.
struct Counter
{
    std::atomic<std::uint32_t> open_regions{0U};
    std::atomic<std::uint64_t> allocations{0U};
    std::atomic<const void*> retained{nullptr};
};

[[nodiscard]] Counter& counter() noexcept
{
    constinit static Counter instance{};
    return instance;
}

// operator new(0) must return a unique pointer; the C heap may return null for 0.
[[nodiscard]] std::size_t at_least_one(std::size_t size) noexcept
{
    return size == 0U ? 1U : size;
}

void count() noexcept
{
    if (counter().open_regions.load(std::memory_order_relaxed) != 0U)
    {
        counter().allocations.fetch_add(1U, std::memory_order_relaxed);
    }
}

[[nodiscard]] void* allocate(std::size_t size) noexcept
{
    count();
    return std::malloc(at_least_one(size)); // NOLINT(cppcoreguidelines-no-malloc,cppcoreguidelines-owning-memory): the replaceable operator new
}

[[nodiscard]] void* allocate_aligned(std::size_t size, std::align_val_t alignment) noexcept
{
    count();
    // The boundary's own type: std::align_val_t is an enum of std::size_t ([new.syn]).
    const auto bytes = static_cast<std::size_t>(alignment);
#if defined(_WIN32)
    return _aligned_malloc(at_least_one(size), bytes);
#else
    const std::size_t rounded = (at_least_one(size) + bytes - 1U) / bytes * bytes;
    return std::aligned_alloc(bytes, rounded); // NOLINT(cppcoreguidelines-no-malloc,cppcoreguidelines-owning-memory): the replaceable operator new
#endif
}

void release(void* pointer) noexcept
{
    std::free(pointer); // NOLINT(cppcoreguidelines-no-malloc,cppcoreguidelines-owning-memory): the replaceable operator delete
}

void release_aligned(void* pointer) noexcept
{
#if defined(_WIN32)
    _aligned_free(pointer);
#else
    std::free(pointer); // NOLINT(cppcoreguidelines-no-malloc,cppcoreguidelines-owning-memory): the replaceable operator delete
#endif
}

// The throwing forms cannot throw in this binary (-fno-exceptions); a failed
// allocation ends the run, as the library's own would.
[[nodiscard]] void* allocate_or_abort(void* pointer) noexcept
{
    if (pointer == nullptr)
    {
        std::abort();
    }

    return pointer;
}

} // namespace

namespace signal_lab_test
{

CountedRegion::CountedRegion() noexcept : mStart{counter().allocations.load(std::memory_order_relaxed)}

{
    counter().open_regions.fetch_add(1U, std::memory_order_relaxed);
}

CountedRegion::~CountedRegion()
{
    counter().open_regions.fetch_sub(1U, std::memory_order_relaxed);
}

std::uint64_t CountedRegion::allocations() const noexcept
{
    return counter().allocations.load(std::memory_order_relaxed) - mStart;
}

std::uint64_t counted_allocations() noexcept
{
    return counter().allocations.load(std::memory_order_relaxed);
}

void retain(const void* pointer) noexcept
{
    counter().retained.store(pointer, std::memory_order_relaxed);
}

} // namespace signal_lab_test

// NOLINTBEGIN(cppcoreguidelines-owning-memory,misc-new-delete-overloads): the replaceable global allocation functions
void* operator new(std::size_t size)
{
    return allocate_or_abort(allocate(size));
}

void* operator new[](std::size_t size)
{
    return allocate_or_abort(allocate(size));
}

void* operator new(std::size_t size, const std::nothrow_t& /*tag*/) noexcept
{
    return allocate(size);
}

void* operator new[](std::size_t size, const std::nothrow_t& /*tag*/) noexcept
{
    return allocate(size);
}

void* operator new(std::size_t size, std::align_val_t alignment)
{
    return allocate_or_abort(allocate_aligned(size, alignment));
}

void* operator new[](std::size_t size, std::align_val_t alignment)
{
    return allocate_or_abort(allocate_aligned(size, alignment));
}

void* operator new(std::size_t size, std::align_val_t alignment, const std::nothrow_t& /*tag*/) noexcept
{
    return allocate_aligned(size, alignment);
}

void* operator new[](std::size_t size, std::align_val_t alignment, const std::nothrow_t& /*tag*/) noexcept
{
    return allocate_aligned(size, alignment);
}

void operator delete(void* pointer) noexcept
{
    release(pointer);
}

void operator delete[](void* pointer) noexcept
{
    release(pointer);
}

void operator delete(void* pointer, std::size_t /*size*/) noexcept
{
    release(pointer);
}

void operator delete[](void* pointer, std::size_t /*size*/) noexcept
{
    release(pointer);
}

void operator delete(void* pointer, const std::nothrow_t& /*tag*/) noexcept
{
    release(pointer);
}

void operator delete[](void* pointer, const std::nothrow_t& /*tag*/) noexcept
{
    release(pointer);
}

void operator delete(void* pointer, std::align_val_t /*alignment*/) noexcept
{
    release_aligned(pointer);
}

void operator delete[](void* pointer, std::align_val_t /*alignment*/) noexcept
{
    release_aligned(pointer);
}

void operator delete(void* pointer, std::size_t /*size*/, std::align_val_t /*alignment*/) noexcept
{
    release_aligned(pointer);
}

void operator delete[](void* pointer, std::size_t /*size*/, std::align_val_t /*alignment*/) noexcept
{
    release_aligned(pointer);
}

void operator delete(void* pointer, std::align_val_t /*alignment*/, const std::nothrow_t& /*tag*/) noexcept
{
    release_aligned(pointer);
}

void operator delete[](void* pointer, std::align_val_t /*alignment*/, const std::nothrow_t& /*tag*/) noexcept
{
    release_aligned(pointer);
}

// NOLINTEND(cppcoreguidelines-owning-memory,misc-new-delete-overloads)
