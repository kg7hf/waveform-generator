// SPDX-License-Identifier: GPL-3.0-only
// SPDX-FileCopyrightText: 2026 Paul R. Decker

// Host stand-in for the NXP SDMMC host layer (host/usdhc/fsl_sdmmc_host.h) as
// rt1170/platform/waveform_sd.c uses it: the host descriptor it fills in, the
// host's own card detect (SDMMCHOST_CardDetectStatus) and the DATA3 detect
// switch. sd_socket_host.c defines the functions and the controller.

#pragma once

#include "fsl_common.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

// One USDHC controller. The stand-in reads and writes none of its
// registers, so it hands the controller out as const.
typedef struct
{
    uint32_t index;
} USDHC_Type;

extern const USDHC_Type* const USDHC1;

typedef struct
{
    const USDHC_Type* base;
    uint32_t sourceClock_Hz;
} usdhc_host_t;

typedef struct
{
    usdhc_host_t hostController;
    uint32_t* dmaDesBuffer;
    uint32_t dmaDesBufferWordsNum;
} sdmmchost_t;

enum
{
    SDMMCHOST_DMA_DESCRIPTOR_BUFFER_ALIGN_SIZE = 4U,
};

uint32_t SDMMCHOST_CardDetectStatus(sdmmchost_t* host);
void USDHC_CardDetectByData3(const USDHC_Type* base, bool enable);

#ifdef __cplusplus
}
#endif
