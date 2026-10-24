// SPDX-License-Identifier: GPL-3.0-only
// SPDX-FileCopyrightText: 2026 Paul R. Decker

// Host stand-in for the FatFs SD disk glue that rt1170/platform/waveform_msc.c
// uses (source/fsl_sd_disk/fsl_sd_disk.h). media_msc_host.c defines the card
// and the function.

#pragma once

#include "diskio.h"
#include "fsl_sd.h"

#ifdef __cplusplus
extern "C"
{
#endif

extern sd_card_t g_sd;

DSTATUS sd_disk_initialize(BYTE pdrv);

#ifdef __cplusplus
}
#endif
