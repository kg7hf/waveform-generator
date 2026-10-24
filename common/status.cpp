// SPDX-License-Identifier: GPL-3.0-only
// SPDX-FileCopyrightText: 2026 Paul R. Decker

#include "common/status.hpp"

#include <bit>
#include <cstdint>

namespace m110
{

namespace
{

// The handler set_critical_error_handler() installed; none until then.
[[nodiscard]] CriticalErrorHandler& installed_handler() noexcept
{
    static constinit CriticalErrorHandler handler{};
    return handler;
}

// The site of the call that returns to `return_address`
// (critical_site_of()). Its conversion to an integer is the compiler
// boundary's one cast.
[[nodiscard]] CriticalSite site_of(const void* return_address) noexcept
{
    return critical_site_of(std::bit_cast<std::uintptr_t>(return_address));
}

} // namespace

void set_critical_error_handler(CriticalErrorHandler handler) noexcept
{
    installed_handler() = handler;
}

// Never inlined, so its return address is in the function that called it.
[[gnu::noinline]] void critical_error(Status status) noexcept
{
#if defined(__GNUC__) || defined(__clang__)
    const CriticalSite site = site_of(__builtin_return_address(0));
#else
    const CriticalSite site{};
#endif

    if (const CriticalErrorHandler handler = installed_handler(); handler != nullptr)
    {
        handler(status, site);
    }

#if defined(__GNUC__) || defined(__clang__)
    __builtin_trap();
#elif defined(_MSC_VER)
    __debugbreak();
#endif

    for (;;)
    {
    }
}

} // namespace m110
