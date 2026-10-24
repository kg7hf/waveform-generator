// SPDX-License-Identifier: GPL-3.0-only
// SPDX-FileCopyrightText: 2026 Paul R. Decker

// The player's SD clocks (rt1170/platform/waveform_sd_clock.h) against the
// SDK's divider rule: the bus clock USDHC_SetSdClock sets for each request
// SD_CardInit makes, from the default 198 MHz root and from the 24 MHz
// fallback root, and the SD bus time of one 16 KB MSC chunk. Including the
// header also runs its static_asserts on the host.

#include "rt1170/platform/waveform_sd_clock.h"

#include <cstdint>
#include <cstdio>

namespace
{

class Checks
{
public:
    void expect(bool condition, const char* what) noexcept
    {
        if (!condition)
        {
            ++failures_;
            std::fputs("FAIL: ", stdout);
            std::fputs(what, stdout);
            std::fputs("\n", stdout);
        }
    }

    [[nodiscard]] int failures() const noexcept { return failures_; }

private:
    int failures_{};
};

struct Hertz
{
    std::uint32_t value{};

    [[nodiscard]] constexpr bool operator==(const Hertz&) const noexcept = default;
};

// The SDK's SD clock prescaler is 2 to 256 in powers of two, the divisor 1 to
// 16 (USDHC_MAX_CLKFS, USDHC_MAX_DVS).
constexpr std::uint32_t max_prescaler = 256U;
constexpr std::uint32_t max_divisor = 16U;

// USDHC_SetSdClock (fsl_usdhc.c, MCUXpresso SDK 2.16) for an SDR bus, step by
// step: clamp the request to the source, take the smallest total divider whose
// result does not exceed the request, then split it into a power-of-two
// prescaler and a divisor, rounding the total up when they cannot make it.
[[nodiscard]] constexpr Hertz usdhc_bus_clock(Hertz source, Hertz request) noexcept
{
    const std::uint32_t target = request.value > source.value ? source.value : request.value;
    std::uint32_t total = source.value / target;

    if (source.value / total > target)
    {
        ++total;
    }

    std::uint32_t prescaler = total;
    std::uint32_t divisor = 1U;

    if (total > max_divisor)
    {
        prescaler = total / max_divisor;

        while ((max_prescaler % prescaler) != 0U || prescaler == 1U)
        {
            ++prescaler;
        }

        divisor = total / prescaler;

        while (divisor * prescaler < total)
        {
            ++divisor;

            if (divisor > max_divisor)
            {
                prescaler <<= 1U;
                divisor = total / prescaler;
            }
        }
    }
    else if ((total % 2U) != 0U && total != 1U)
    {
        prescaler = 1U;
        divisor = total;
    }

    return Hertz{source.value / divisor / prescaler};
}

constexpr Hertz pll2_root{wfg_sd_usdhc1_pll2_root_hz};
constexpr Hertz boot_root{wfg_sd_usdhc1_boot_root_hz};
constexpr Hertz pfd2{wfg_sd_sys_pll2_pfd2_hz};
constexpr Hertz pll2_bus{wfg_sd_pll2_bus_hz};
constexpr Hertz boot_bus{wfg_sd_boot_bus_hz};

// SD_CardInit's requests at 3.3 V: identification, then Default Speed until
// CMD6 switches the card to High Speed (fsl_sd.c, SD_CLOCK_25MHZ and
// SD_CLOCK_50MHZ).
constexpr Hertz identification_request{wfg_sd_identification_request_hz};
constexpr Hertz default_speed_request{25'000'000U};
constexpr Hertz high_speed_request{wfg_sd_high_speed_max_hz};
constexpr Hertz identification_min{wfg_sd_identification_min_hz};
constexpr Hertz identification_max{wfg_sd_identification_max_hz};

// The SDK's results, worked by hand from fsl_usdhc.c.
constexpr Hertz default_speed_from_pll2{24'750'000U};     // 198 MHz / 8
constexpr Hertz earlier_identification_request{100'000U}; // every player image before this one
constexpr Hertz identification_from_boot{100'000U};       // 24 MHz / (16 * 15)
constexpr Hertz earlier_request_from_pll2{96'679U};       // 198 MHz / (128 * 16)

// One 512-byte block on 4 data lines: 1024 data clocks, a 16-bit CRC per line,
// a start and an end bit. A 16 KB chunk is 32 blocks.
constexpr std::uint64_t clocks_per_block = 1024U + 16U + 2U;
constexpr std::uint64_t blocks_per_chunk = 32U;
constexpr std::uint64_t microseconds_per_second = 1'000'000U;
constexpr std::uint64_t pll2_chunk_bus_microseconds = 673U;
constexpr std::uint64_t boot_chunk_bus_microseconds = 1389U;

[[nodiscard]] constexpr std::uint64_t chunk_bus_microseconds(Hertz bus) noexcept
{
    return clocks_per_block * blocks_per_chunk * microseconds_per_second / bus.value;
}

void test_high_speed(Checks& checks)
{
    checks.expect(usdhc_bus_clock(pll2_root, high_speed_request) == pll2_bus, "High Speed from the 198 MHz root is 49.5 MHz");
    checks.expect(usdhc_bus_clock(boot_root, high_speed_request) == boot_bus, "High Speed from the 24 MHz root stays 24 MHz");
    checks.expect(usdhc_bus_clock(pll2_root, default_speed_request) == default_speed_from_pll2, "Default Speed from the 198 MHz root is 24.75 MHz, within 25 MHz");
    checks.expect(usdhc_bus_clock(pfd2, high_speed_request) == pll2_bus, "the undivided 396 MHz PFD would give the same 49.5 MHz bus");
    checks.expect(Hertz{wfg_sd_image_bus_hz} == pll2_bus, "a build without WFG_SD_CLOCK_24MHZ (WFG_SD_USE_BOOTCLOCKRUN_USDHC1) names the 49.5 MHz bus as its own");
}

void test_identification(Checks& checks)
{
    const Hertz from_pll2 = usdhc_bus_clock(pll2_root, identification_request);
    checks.expect(from_pll2 == identification_request, "identification from the 198 MHz root is 103125 Hz");
    checks.expect(from_pll2.value >= identification_min.value && from_pll2.value <= identification_max.value, "identification from the 198 MHz root is within 100 to 400 kHz");
    checks.expect(usdhc_bus_clock(boot_root, identification_request) == identification_from_boot, "identification from the 24 MHz root stays 100 kHz");
    checks.expect(usdhc_bus_clock(boot_root, earlier_identification_request) == identification_from_boot, "the earlier 100 kHz request gave the same 100 kHz from the 24 MHz root");
    checks.expect(usdhc_bus_clock(pll2_root, earlier_identification_request) == earlier_request_from_pll2, "a 100 kHz request would give 96.7 kHz from the 198 MHz root");
}

void test_chunk_bus_time(Checks& checks)
{
    checks.expect(chunk_bus_microseconds(pll2_bus) == pll2_chunk_bus_microseconds, "a 16 KB chunk spends 0.67 ms on the 49.5 MHz bus");
    checks.expect(chunk_bus_microseconds(boot_bus) == boot_chunk_bus_microseconds, "a 16 KB chunk spent 1.39 ms on the 24 MHz bus");
}

} // namespace

int main()
{
    Checks checks;
    test_high_speed(checks);
    test_identification(checks);
    test_chunk_bus_time(checks);

    if (checks.failures() != 0)
    {
        std::fputs("SD clock tests: FAIL\n", stdout);
        return 1;
    }

    std::fputs("SD clock tests: PASS\n", stdout);
    return 0;
}
