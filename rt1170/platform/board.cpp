// SPDX-License-Identifier: GPL-3.0-only
// SPDX-FileCopyrightText: 2026 Paul R. Decker

extern "C"
{
#include "board.h"
#include "board_pins.h"
#include "clock_config.h"
#include "fsl_common.h"
#include "fsl_debug_console.h"
#include "fsl_gpio.h"
}

#include "platform/board.hpp"
#include "platform/cycle_counter.h"

namespace m110
{

void platform_initialize() noexcept
{
    const gpio_pin_config_t led_config{kGPIO_DigitalOutput, LOGIC_LED_OFF, kGPIO_NoIntmode};
    BOARD_ConfigMPU();
    BOARD_InitWaveformPins();
    BOARD_BootClockRUN();
    BOARD_InitDebugConsole();
    GPIO_PinInit(BOARD_USER_LED_GPIO, BOARD_USER_LED_GPIO_PIN, &led_config);
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    DWT->CYCCNT = 0U;
    DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
}

std::uint32_t platform_cycle_counter() noexcept
{
    return DWT->CYCCNT;
}

std::uint32_t platform_cycle_counter_frequency_hz() noexcept
{
    return SystemCoreClock;
}

void platform_set_status(PlatformStatus status) noexcept
{
    GPIO_PinWrite(BOARD_USER_LED_GPIO, BOARD_USER_LED_GPIO_PIN, status == PlatformStatus::idle ? LOGIC_LED_OFF : LOGIC_LED_ON);
}

void platform_delay(std::uint32_t milliseconds) noexcept
{
    SDK_DelayAtLeastUs(milliseconds * 1000U, SDK_DEVICE_MAXIMUM_CPU_CLOCK_FREQUENCY);
}

void platform_idle() noexcept
{
    // Keep the CM7 awake until the RT1170 power policy explicitly configures a
    // debugger-visible sleep mode and wake source.  Entering WFI here makes the
    // otherwise empty application skeleton difficult to reconnect to reliably.
    __NOP();
}

void platform_write_diagnostic(const char* text) noexcept
{
    if (text != nullptr)
    {
        PRINTF("%s", text);
    }
}

void platform_mask_interrupts() noexcept
{
    __disable_irq();
}

// CMSIS takes the range's length as an int32_t: the C API's one cast.
void platform_write_back(std::span<std::byte> bytes) noexcept
{
    SCB_CleanDCache_by_Addr(bytes.data(), static_cast<std::int32_t>(bytes.size()));
}

} // namespace m110

// The cycle counter's C entry points (platform/cycle_counter.h), for C code
// such as the MSC transfer timing in waveform_msc.c. The header gives them C
// linkage.
std::uint32_t m110_platform_cycle_counter()
{
    return m110::platform_cycle_counter();
}

std::uint32_t m110_platform_cycle_counter_frequency_hz()
{
    return m110::platform_cycle_counter_frequency_hz();
}
