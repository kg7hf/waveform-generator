// SPDX-License-Identifier: GPL-3.0-only
// SPDX-FileCopyrightText: 2026 Paul R. Decker

// Host stand-ins for what rt1170/platform/waveform_sd.c calls: NXP's clock,
// GPIO and IOMUXC drivers, the SD host layer's card detect and DATA3 switch,
// the busy-wait delay and the interrupt priority. Each does nothing but what
// tests/sd_socket_tests.cpp reads through sd_socket_host.h: the host's card
// detect answers what the test set, and counts its calls. The reads the test
// asks for go through a card WFG_SD_Config() set up, each with its own copy
// of the card detect where it needs another, so the socket's own stays as
// WFG_SD_Config() left it.

#include "sd_socket_host.h"

#include "fsl_clock.h"
#include "fsl_common.h"
#include "fsl_gpio.h"
#include "fsl_iomuxc.h"
#include "fsl_sdmmc_host.h"
#include "waveform_sd.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum
{
    // The player's USDHC1 root (waveform_sd_clock.h): SYS_PLL2_PFD2 / 2.
    usdhc1_root_hz = 198000000,
};

typedef struct
{
    uint32_t host_card_detect;
    uint32_t host_card_detect_reads;
} socket_model_t;

static socket_model_t* model(void)
{
    static socket_model_t instance;
    return &instance;
}

static const USDHC_Type usdhc1 = {1U};
static const GPIO_Type gpio3 = {3U};
static const GPIO_Type cm7_gpio3 = {3U};
static const GPIO_Type gpio10 = {10U};

const USDHC_Type* const USDHC1 = &usdhc1;
const GPIO_Type* const GPIO3 = &gpio3;
const GPIO_Type* const CM7_GPIO3 = &cm7_gpio3;
const GPIO_Type* const GPIO10 = &gpio10;

// A GPIO card detect that finds no card.
static bool no_card_on_gpio(void)
{
    return false;
}

// A card WFG_SD_Config() set up, its host readied by SD_HostInit().
static sd_card_t configured_card(void)
{
    sd_card_t card = {0};
    WFG_SD_Config(&card);
    card.isHostReady = true;
    return card;
}

bool wfg_test_socket_detects_by_gpio(void)
{
    const sd_card_t card = configured_card();
    return card.usrParam.cd != NULL && card.usrParam.cd->type == kSD_DetectCardByGpioCD;
}

enum wfg_status wfg_test_read_socket_detect(uint32_t* status)
{
    sd_card_t card = configured_card();
    return WFG_SD_ReadHostDetectStatus(&card, status);
}

// A read through a copy of the socket's card detect of `type`, whose callback is `detected`.
static __attribute__((warn_unused_result)) enum wfg_status read_through(sd_detect_card_type_t type, sd_cd_status_t detected, uint32_t* status)
{
    sd_card_t card = configured_card();
    sd_detect_card_t detect = *card.usrParam.cd;
    detect.type = type;
    detect.cardDetected = detected;
    card.usrParam.cd = &detect;
    return WFG_SD_ReadHostDetectStatus(&card, status);
}

enum wfg_status wfg_test_read_gpio_detect_without_card(uint32_t* status)
{
    return read_through(kSD_DetectCardByGpioCD, no_card_on_gpio, status);
}

enum wfg_status wfg_test_read_gpio_detect_without_callback(uint32_t* status)
{
    return read_through(kSD_DetectCardByGpioCD, NULL, status);
}

enum wfg_status wfg_test_read_host_data3_detect(uint32_t* status)
{
    return read_through(kSD_DetectCardByHostDATA3, NULL, status);
}

enum wfg_status wfg_test_read_without_detect(uint32_t* status)
{
    sd_card_t card = configured_card();
    card.usrParam.cd = NULL;
    return WFG_SD_ReadHostDetectStatus(&card, status);
}

enum wfg_status wfg_test_read_without_card(uint32_t* status)
{
    return WFG_SD_ReadHostDetectStatus(NULL, status);
}

enum wfg_status wfg_test_read_without_status(void)
{
    sd_card_t card = configured_card();
    return WFG_SD_ReadHostDetectStatus(&card, NULL);
}

enum wfg_status wfg_test_read_host_not_ready(uint32_t* status)
{
    sd_card_t card = configured_card();
    card.isHostReady = false;
    return WFG_SD_ReadHostDetectStatus(&card, status);
}

enum wfg_status wfg_test_read_without_host(uint32_t* status)
{
    sd_card_t card = configured_card();
    card.host = NULL;
    return WFG_SD_ReadHostDetectStatus(&card, status);
}

void wfg_test_set_host_card_detect(uint32_t status)
{
    model()->host_card_detect = status;
}

uint32_t wfg_test_host_card_detect_reads(void)
{
    return model()->host_card_detect_reads;
}

uint32_t SDMMCHOST_CardDetectStatus(sdmmchost_t* host)
{
    (void)host;
    model()->host_card_detect_reads++;
    return model()->host_card_detect;
}

void USDHC_CardDetectByData3(const USDHC_Type* base, bool enable)
{
    (void)base;
    (void)enable;
}

void CLOCK_SetRootClock(clock_root_t root, const clock_root_config_t* config)
{
    (void)root;
    (void)config;
}

uint32_t CLOCK_GetRootClockFreq(clock_root_t root)
{
    (void)root;
    return usdhc1_root_hz;
}

void GPIO_PinInit(const GPIO_Type* base, uint32_t pin, const gpio_pin_config_t* config)
{
    (void)base;
    (void)pin;
    (void)config;
}

uint32_t GPIO_PinRead(const GPIO_Type* base, uint32_t pin)
{
    (void)base;
    (void)pin;
    return 0U;
}

void GPIO_PinWrite(const GPIO_Type* base, uint32_t pin, uint8_t output)
{
    (void)base;
    (void)pin;
    (void)output;
}

void IOMUXC_SetPinConfig(uint32_t muxRegister, uint32_t muxMode, uint32_t inputRegister, uint32_t inputDaisy, uint32_t configRegister,
                         uint32_t configValue)
{
    (void)muxRegister;
    (void)muxMode;
    (void)inputRegister;
    (void)inputDaisy;
    (void)configRegister;
    (void)configValue;
}

void SDK_DelayAtLeastUs(uint32_t delay_us, uint32_t core_clock_hz)
{
    (void)delay_us;
    (void)core_clock_hz;
}

void NVIC_SetPriority(IRQn_Type irq, uint32_t priority)
{
    (void)irq;
    (void)priority;
}
