// SPDX-License-Identifier: GPL-3.0-only
// SPDX-FileCopyrightText: 2026 Paul R. Decker

// Host stand-in for the NXP SD card API's types (sd/fsl_sd.h and
// common/fsl_sdmmc_common.h) that rt1170/platform/waveform_sd.c and
// waveform_sd.h use: the card descriptor, its user parameters and the card
// detect, with the SDK's names, values and field order.

#pragma once

#include "fsl_sdmmc_host.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

// SD_DetectCardInsert() results.
enum
{
    kSD_Removed = 0U,
    kSD_Inserted = 1U,
};

enum
{
    kSD_DAT3PullDown = 0U,
    kSD_DAT3PullUp = 1U,
};

typedef enum
{
    kSD_DetectCardByGpioCD,
    kSD_DetectCardByHostCD,
    kSD_DetectCardByHostDATA3,
} sd_detect_card_type_t;

typedef void (*sd_cd_t)(bool isInserted, void* userData);
typedef bool (*sd_cd_status_t)(void);
typedef void (*sd_dat3_pull_t)(uint32_t pullStatus);

typedef struct
{
    sd_detect_card_type_t type;
    uint32_t cdDebounce_ms;
    sd_cd_t callback;
    sd_cd_status_t cardDetected;
    sd_dat3_pull_t dat3PullFunc;
    void* userData;
} sd_detect_card_t;

typedef enum
{
    kSD_IOVoltageCtrlNotSupport = 0U,
    kSD_IOVoltageCtrlByHost = 1U,
    kSD_IOVoltageCtrlByGpio = 2U,
} sd_io_voltage_ctrl_type_t;

typedef void (*sd_io_voltage_func_t)(uint32_t voltage);

typedef struct
{
    sd_io_voltage_ctrl_type_t type;
    sd_io_voltage_func_t func;
} sd_io_voltage_t;

typedef void (*sd_pwr_t)(bool enable);
typedef void (*sd_io_strength_t)(uint32_t busFreq);

typedef struct
{
    sd_pwr_t pwr;
    uint32_t powerOnDelayMS;
    uint32_t powerOffDelayMS;
    sd_io_strength_t ioStrength;
    sd_io_voltage_t* ioVoltage;
    sd_detect_card_t* cd;
    uint32_t maxFreq;
    uint32_t capability;
} sd_usr_param_t;

typedef struct
{
    sdmmchost_t* host;
    sd_usr_param_t usrParam;
    bool isHostReady;
} sd_card_t;

#ifdef __cplusplus
}
#endif
