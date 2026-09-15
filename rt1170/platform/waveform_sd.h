// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Paul R. Decker

#pragma once

#include "fsl_sd.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

typedef struct wfg_sd_detect_sample
{
    uint32_t gpio3_level;
    uint32_t cm7_gpio3_level;
    uint32_t inserted;
} wfg_sd_detect_sample_t;

/* Configure the MIMXRT1170-EVK microSD socket for the generator player. */
void WFG_SD_Config(sd_card_t* card);
bool WFG_SD_ReadHostDetectStatus(sd_card_t* card, uint32_t* status);
void WFG_SD_PrepareDataTransfer(sd_card_t* card);
uint32_t WFG_SD_SourceClockHz(const sd_card_t* card);
wfg_sd_detect_sample_t WFG_SD_ReadDetectSample(void);

#ifdef __cplusplus
}
#endif
