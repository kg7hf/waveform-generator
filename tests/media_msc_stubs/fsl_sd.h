// SPDX-License-Identifier: GPL-3.0-only
// SPDX-FileCopyrightText: 2026 Paul R. Decker

// Host stand-in for the NXP SDMMC card API that rt1170/platform/waveform_msc.c
// and waveform_sd.h use (sd/fsl_sd.h and common/fsl_sdmmc_common.h).
// media_msc_host.c implements the functions.

#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

typedef int32_t status_t;

enum
{
    kStatus_Success = 0,
    kStatus_Fail = 1,
};

// SD_DetectCardInsert() results.
enum
{
    kSD_Removed = 0U,
    kSD_Inserted = 1U,
};

typedef struct
{
    bool isHostReady;
    uint32_t busClock_Hz; // the SD bus clock the driver last set, in Hz
    uint32_t blockCount;
    uint32_t blockSize;
} sd_card_t;

void SD_Deinit(sd_card_t* card);
status_t SD_CardInit(sd_card_t* card);
status_t SD_HostInit(sd_card_t* card);
void SD_HostDeinit(sd_card_t* card);
void SD_SetCardPower(sd_card_t* card, bool enable);
status_t SD_ReadBlocks(sd_card_t* card, uint8_t* buffer, uint32_t startBlock, uint32_t blockCount);
status_t SD_WriteBlocks(sd_card_t* card, const uint8_t* buffer, uint32_t startBlock, uint32_t blockCount);

#ifdef __cplusplus
}
#endif
