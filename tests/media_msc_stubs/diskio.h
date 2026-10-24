// SPDX-License-Identifier: GPL-3.0-only
// SPDX-FileCopyrightText: 2026 Paul R. Decker

// Host stand-in for the FatFs disk I/O types that rt1170/platform/waveform_msc.c
// uses (source/diskio.h).

#pragma once

#include <stdint.h>

typedef uint8_t BYTE;
typedef BYTE DSTATUS;

// The physical drive FatFs maps to the SD card.
enum
{
    SDDISK = 2,
};
