// SPDX-License-Identifier: GPL-3.0-only
// SPDX-FileCopyrightText: 2026 Paul R. Decker

#include "waveform_sd.h"

#include "fsl_clock.h"
#include "fsl_common.h"
#include "fsl_gpio.h"
#include "fsl_iomuxc.h"
#include "fsl_sdmmc_host.h"
#include "waveform_sd_clock.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * Generator-owned adaptation of NXP's evkmimxrt1170
 * sdcard_fatfs_freertos CM7 board setup from MCUX_2.16.100. The card runs
 * High Speed (SDR25) at 3.3 V within a 50 MHz ceiling: 49.5 MHz from the
 * player's default USDHC1 root, 24 MHz from the BOARD_BootClockRUN root
 * (waveform_sd_clock.h). PCM16 mono playback needs only 96 kB/s; MSC staging
 * uses the rest.
 */

#define WFG_SD_HOST USDHC1
#define WFG_SD_HOST_IRQ USDHC1_IRQn
#if defined(CPU_MIMXRT1064DVL6A)
#define WFG_SD_CARD_DETECT_GPIO GPIO2
#define WFG_SD_CARD_DETECT_PIN 28U
#define WFG_SD_POWER_GPIO GPIO1
#define WFG_SD_POWER_PIN 19U
#define WFG_SD_DATA3_MUX IOMUXC_GPIO_SD_B0_05_USDHC1_DATA3
#else
#define WFG_SD_CARD_DETECT_GPIO CM7_GPIO3
#define WFG_SD_CARD_DETECT_PIN 31U
#define WFG_SD_POWER_GPIO GPIO10
#define WFG_SD_POWER_PIN 2U
#define WFG_SD_DATA3_MUX IOMUXC_GPIO_SD_B1_05_USDHC1_DATA3
#endif
#define WFG_SD_DMA_DESCRIPTOR_WORDS 32U
#define WFG_SD_MAX_FREQUENCY_HZ 50000000U
#define WFG_SD_HOST_IRQ_PRIORITY 7U
#if defined(CPU_MIMXRT1064DVL6A)
#define WFG_SD_DAT3_DETECT_PULL_DOWN_PAD 0x130F9U
#define WFG_SD_DAT3_TRANSFER_PAD 0x170F9U
#else
#define WFG_SD_DAT3_DETECT_PULL_DOWN_PAD 0x0CU
#define WFG_SD_DAT3_TRANSFER_PAD 0x04U
#endif

AT_NONCACHEABLE_SECTION_ALIGN(
    static uint32_t s_sdmmc_dma_descriptors[WFG_SD_DMA_DESCRIPTOR_WORDS],
    SDMMCHOST_DMA_DESCRIPTOR_BUFFER_ALIGN_SIZE);

static sd_detect_card_t s_card_detect;
static sd_io_voltage_t s_io_voltage = {
#if defined(WFG_SD_DISABLE_HOST_VOLTAGE_CONTROL)
    .type = kSD_IOVoltageCtrlNotSupport,
#else
    .type = kSD_IOVoltageCtrlByHost,
#endif
    .func = NULL,
};
static sdmmchost_t s_host;

#if defined(WFG_SD_ALWAYS_PRESENT_CARD_DETECT)
static bool WFG_SD_CardDetected(void)
{
#if defined(CPU_MIMXRT1064DVL6A)
    return GPIO_PinRead(WFG_SD_CARD_DETECT_GPIO, WFG_SD_CARD_DETECT_PIN) == 0U;
#else
    return true;
#endif
}
#endif

#if defined(WFG_PLAYER_IMAGE)
_Static_assert(WFG_SD_IDENTIFICATION_CLOCK_HZ == wfg_sd_identification_request_hz,
               "rt1170/CMakeLists.txt asks for the identification clock waveform_sd_clock.h checks");
#endif

/* The USDHC1 root clock, which the SDK divides down to the card's bus clock
   (its USDHC_SetSdClock, from the frequency returned here). By default
   SYS_PLL2_PFD2 / 2 = 198 MHz, so High Speed runs at 49.5 MHz.
   BOARD_BootClockRUN already runs SYS_PLL2 and its PFD2 (396 MHz) and owns
   them: only this root's mux and divider change here, while the SD host is
   not initialised (WFG_SD_Config precedes SD_HostInit). NXP's sequence also
   re-runs CLOCK_InitSysPll2 and CLOCK_InitPfd, which return at once while
   BOARD_BootClockRUN's settings hold, and would otherwise retune the PLL
   under every other clock it feeds. WFG_SD_USE_BOOTCLOCKRUN_USDHC1 (CMake
   WFG_SD_CLOCK_24MHZ, and the SD-only image) keeps BOARD_BootClockRUN's root,
   OSC_RC_48M_DIV2 = 24 MHz, and the bus at 24 MHz. */
static uint32_t WFG_SD_ClockConfiguration(void)
{
#if defined(CPU_MIMXRT1064DVL6A)
    /* BootClockRUN selects SYS PLL PFD2 (396 MHz) / 2 = 198 MHz. */
    return CLOCK_GetClockRootFreq(kCLOCK_Usdhc1ClkRoot);
#else
#if !defined(WFG_SD_USE_BOOTCLOCKRUN_USDHC1)
    const clock_root_config_t root_config = {
        .clockOff = false,
        .mux = kCLOCK_USDHC1_ClockRoot_MuxSysPll2Pfd2,
        .div = wfg_sd_usdhc1_pll2_root_divider,
    };
    CLOCK_SetRootClock(kCLOCK_Root_Usdhc1, &root_config);
#endif
    return CLOCK_GetRootClockFreq(kCLOCK_Root_Usdhc1);
#endif
}

wfg_sd_detect_sample_t WFG_SD_ReadDetectSample(void)
{
    wfg_sd_detect_sample_t sample;
#if defined(CPU_MIMXRT1064DVL6A)
    sample.gpio3_level = GPIO_PinRead(WFG_SD_CARD_DETECT_GPIO, WFG_SD_CARD_DETECT_PIN);
    sample.cm7_gpio3_level = sample.gpio3_level;
#else
    sample.gpio3_level = GPIO_PinRead(GPIO3, WFG_SD_CARD_DETECT_PIN);
    sample.cm7_gpio3_level = GPIO_PinRead(CM7_GPIO3, WFG_SD_CARD_DETECT_PIN);
#endif
    sample.inserted = sample.cm7_gpio3_level == 0U ? 1U : 0U;
    return sample;
}

static void WFG_SD_PowerControl(bool enable)
{
#if defined(CPU_MIMXRT1064DVL6A)
    GPIO_PinWrite(WFG_SD_POWER_GPIO, WFG_SD_POWER_PIN, enable ? 1U : 0U);
#else
    /* The RT1170 EVK socket power switch is active low. */
    GPIO_PinWrite(WFG_SD_POWER_GPIO, WFG_SD_POWER_PIN, enable ? 0U : 1U);
#endif
}

static void WFG_SD_DAT3PullFunction(uint32_t status)
{
    if (status == kSD_DAT3PullDown)
    {
        IOMUXC_SetPinConfig(WFG_SD_DATA3_MUX,
                            WFG_SD_DAT3_DETECT_PULL_DOWN_PAD);
        WFG_SD_PowerControl(false);
        SDK_DelayAtLeastUs(1000U, SDK_DEVICE_MAXIMUM_CPU_CLOCK_FREQUENCY);
        WFG_SD_PowerControl(true);
        SDK_DelayAtLeastUs(1000U, SDK_DEVICE_MAXIMUM_CPU_CLOCK_FREQUENCY);
    }
    else
    {
        IOMUXC_SetPinConfig(WFG_SD_DATA3_MUX,
                            WFG_SD_DAT3_TRANSFER_PAD);
    }
}

void WFG_SD_Config(sd_card_t* card)
{
    const gpio_pin_config_t detect_config = {
        kGPIO_DigitalInput,
        0U,
        kGPIO_NoIntmode,
    };
    const gpio_pin_config_t power_config = {
        kGPIO_DigitalOutput,
        1U,
        kGPIO_NoIntmode,
    };

    if (card == NULL)
    {
        return;
    }

#if defined(WFG_SD_ALWAYS_PRESENT_CARD_DETECT)
    s_card_detect.type = kSD_DetectCardByGpioCD;
#else
    s_card_detect.type = kSD_DetectCardByHostDATA3;
#endif
    s_card_detect.cdDebounce_ms = 100U;
    s_card_detect.callback = NULL;
#if defined(WFG_SD_ALWAYS_PRESENT_CARD_DETECT)
    s_card_detect.cardDetected = WFG_SD_CardDetected;
#else
    s_card_detect.cardDetected = NULL;
#endif
    s_card_detect.dat3PullFunc = WFG_SD_DAT3PullFunction;
    s_card_detect.userData = NULL;

    s_host.dmaDesBuffer = s_sdmmc_dma_descriptors;
    s_host.dmaDesBufferWordsNum = WFG_SD_DMA_DESCRIPTOR_WORDS;
#if ((defined(__DCACHE_PRESENT) && (__DCACHE_PRESENT != 0U)) || \
     (defined(FSL_FEATURE_HAS_L1CACHE) && (FSL_FEATURE_HAS_L1CACHE != 0U)))
    s_host.enableCacheControl = kSDMMCHOST_CacheControlRWBuffer;
#endif

    card->host = &s_host;
    card->host->hostController.base = WFG_SD_HOST;
    card->host->hostController.sourceClock_Hz = WFG_SD_ClockConfiguration();
    card->usrParam.pwr = WFG_SD_PowerControl;
    card->usrParam.ioStrength = NULL;
    card->usrParam.ioVoltage = &s_io_voltage;
    card->usrParam.cd = &s_card_detect;
    card->usrParam.maxFreq = WFG_SD_MAX_FREQUENCY_HZ;
#if defined(WFG_SD_ALWAYS_PRESENT_CARD_DETECT)
    card->usrParam.powerOffDelayMS = 1000U;
    card->usrParam.powerOnDelayMS = 1000U;
#endif

    GPIO_PinInit(WFG_SD_CARD_DETECT_GPIO, WFG_SD_CARD_DETECT_PIN, &detect_config);
    GPIO_PinInit(WFG_SD_POWER_GPIO, WFG_SD_POWER_PIN, &power_config);
    WFG_SD_PowerControl(true);

    NVIC_SetPriority(WFG_SD_HOST_IRQ, WFG_SD_HOST_IRQ_PRIORITY);

#if defined(CPU_MIMXRT1176DVMAA_cm7)
    /* ERR050396: keep uSDHC AXI writes non-cacheable when targeting CM7 TCM. */
    IOMUXC_GPR->GPR28 &= ~IOMUXC_GPR_GPR28_AWCACHE_USDHC_MASK;
#endif
}

enum wfg_status WFG_SD_ReadHostDetectStatus(sd_card_t* card, uint32_t* detect_status)
{
    if (card == NULL || detect_status == NULL)
    {
        return kWFG_StatusInvalidArgument;
    }

    if (card->isHostReady == false || card->host == NULL)
    {
        return kWFG_StatusSdHostNotReady;
    }

    if (card->usrParam.cd != NULL && card->usrParam.cd->type == kSD_DetectCardByGpioCD)
    {
        *detect_status = card->usrParam.cd->cardDetected != NULL && card->usrParam.cd->cardDetected() ? kSD_Inserted : kSD_Removed;
    }
    else
    {
        *detect_status = SDMMCHOST_CardDetectStatus(card->host);
    }
    return kWFG_StatusOk;
}

void WFG_SD_PrepareDataTransfer(sd_card_t* card)
{
    if (card != NULL && card->host != NULL)
    {
        USDHC_CardDetectByData3(card->host->hostController.base, false);
    }
    IOMUXC_SetPinConfig(WFG_SD_DATA3_MUX,
                        WFG_SD_DAT3_TRANSFER_PAD);
}

uint32_t WFG_SD_SourceClockHz(const sd_card_t* card)
{
    if (card != NULL && card->host != NULL)
    {
        return card->host->hostController.sourceClock_Hz;
    }

#if defined(CPU_MIMXRT1064DVL6A)
    return CLOCK_GetClockRootFreq(kCLOCK_Usdhc1ClkRoot);
#else
    return CLOCK_GetRootClockFreq(kCLOCK_Root_Usdhc1);
#endif
}
