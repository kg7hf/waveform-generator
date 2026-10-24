// SPDX-License-Identifier: GPL-3.0-only
// SPDX-FileCopyrightText: 2026 Paul R. Decker

// Host stand-in for the NXP GPIO driver (drivers/fsl_gpio.h) as
// rt1170/platform/waveform_sd.c uses it: the socket's card-detect input and
// power switch. sd_socket_host.c defines the ports and the functions.

#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

// One GPIO port. The stand-in reads and writes none of its registers, so
// it hands the ports out as const.
typedef struct
{
    uint32_t index;
} GPIO_Type;

extern const GPIO_Type* const GPIO3;
extern const GPIO_Type* const CM7_GPIO3;
extern const GPIO_Type* const GPIO10;

typedef enum
{
    kGPIO_DigitalInput = 0U,
    kGPIO_DigitalOutput = 1U,
} gpio_pin_direction_t;

typedef enum
{
    kGPIO_NoIntmode = 0U,
} gpio_interrupt_mode_t;

typedef struct
{
    gpio_pin_direction_t direction;
    uint8_t outputLogic;
    gpio_interrupt_mode_t interruptMode;
} gpio_pin_config_t;

void GPIO_PinInit(const GPIO_Type* base, uint32_t pin, const gpio_pin_config_t* config);
uint32_t GPIO_PinRead(const GPIO_Type* base, uint32_t pin);
void GPIO_PinWrite(const GPIO_Type* base, uint32_t pin, uint8_t output);

#ifdef __cplusplus
}
#endif
